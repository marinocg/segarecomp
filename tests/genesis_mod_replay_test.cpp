// SEG-138-T007 (ADR 0299): replay, input determinism, sandbox failure and incompatible-state regression suite for Genesis mods.
//
// A project-authored synthetic program runs under the ONE mod scheduler with a mod that intercepts input, writes work RAM and keeps state, once as a native ABI 2 provider and once as a Luau mod.
// A scripted (recorded) pad sequence is fed identically each time. Checked: replay equivalence (identical per-frame state hashes), restore equivalence under replay, deterministic sandbox
// failure during replay (capability/range, Luau budget and Luau error faults, replayed across a restore), and incompatible-state rejection that never partially applies. Everything is bounded
// (<= 40 frames, a few hundred restores) and uses no ROM, wall clock, host RNG or commercial input.
#include "genesis_snapshot.h"
#include "mod_scheduler.h"
#include "runtime.h"
#include "segarecomp/luau_api.hpp"
#include "segarecomp/luau_phase_provider.hpp"
#include "segarecomp/mod.hpp"
#include "segarecomp/sha256.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
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
constexpr uint64_t cap(unsigned id) { return UINT64_C(1) << id; }
constexpr uint32_t bit(uint32_t phase) { return 1U << phase; }
constexpr uint32_t kPhases = bit(SEGA_MOD_PHASE_FRAME_BEGIN) | bit(SEGA_MOD_PHASE_INPUT) | bit(SEGA_MOD_PHASE_POST_INPUT) | bit(SEGA_MOD_PHASE_FRAME_END);
constexpr uint64_t kCaps = cap(SEGA_MOD_CAP_MEMORY_READ) | cap(SEGA_MOD_CAP_MEMORY_WRITE) | cap(SEGA_MOD_CAP_INPUT_READ) | cap(SEGA_MOD_CAP_INPUT_INTERCEPT) | cap(SEGA_MOD_CAP_STATE_MOD);
const std::vector<std::string> kLuauCaps = {"memory.read.m68k.work_ram", "memory.write.m68k.work_ram", "input.read", "input.intercept", "state.mod"};
const GenesisModRange kRanges[] = {{SEGA_GEN_SPACE_M68K_WORK_RAM, 0x100U, 0x100U, GENESIS_MOD_ACCESS_READ | GENESIS_MOD_ACCESS_WRITE}};

// ---- the synthetic program: reads controller port 1 on every dispatch; the pad byte steers guest state ----
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
  rt->d[1] = value;
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
// The recorded input: a pure function of the frame number (the replay file of the test).
uint8_t recorded_pad(uint64_t frame) { return static_cast<uint8_t>((frame * 37U + 11U) & 0x3EU); }

// ---- the mod, in two implementations of the same behavior ----
enum class Fault { none, range, budget, error };
struct NativeMod {
  uint32_t n = 0;
  uint64_t fault_frame = 0;
  Fault fault = Fault::none;
  uint32_t loads = 0;
};
void native_phase(void *state, const SegaModHost *h) {
  auto *m = static_cast<NativeMod *>(state);
  uint32_t v = 0;
  switch (h->phase) {
    case SEGA_MOD_PHASE_FRAME_BEGIN:
      ++m->n;
      h->write(SEGA_GEN_SPACE_M68K_WORK_RAM, 0x100, 4, m->n);
      if (m->fault == Fault::range && h->frame == m->fault_frame) h->write(SEGA_GEN_SPACE_M68K_WORK_RAM, 0x800, 1, 1);   // outside every declared range
      break;
    case SEGA_MOD_PHASE_INPUT:
      h->input_get(1, &v);
      h->input_set(1, v | 1U);
      break;
    case SEGA_MOD_PHASE_POST_INPUT:
      h->input_get(1, &v);
      h->write(SEGA_GEN_SPACE_M68K_WORK_RAM, 0x110, 2, v);
      break;
    case SEGA_MOD_PHASE_FRAME_END:
      h->read(SEGA_GEN_SPACE_M68K_WORK_RAM, 0x120, 1, &v);
      h->write(SEGA_GEN_SPACE_M68K_WORK_RAM, 0x121, 1, (v + 1U) % 256U);
      break;
    default: break;
  }
}
int native_save(void *state, uint8_t *out, uint32_t cap_bytes, uint32_t *len) {
  const auto *m = static_cast<NativeMod *>(state);
  if (cap_bytes < 4U) return 1;
  for (unsigned i = 0; i < 4U; ++i) out[i] = static_cast<uint8_t>(m->n >> (8U * i));
  *len = 4U;
  return 0;
}
int native_load(void *state, const uint8_t *in, uint32_t len) {
  auto *m = static_cast<NativeMod *>(state);
  ++m->loads;
  if (len != 4U) return 1;
  m->n = static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8U) | (static_cast<uint32_t>(in[2]) << 16U) | (static_cast<uint32_t>(in[3]) << 24U);
  return 0;
}

std::string luau_source(uint64_t fault_frame, Fault fault) {
  std::string fault_code;
  if (fault == Fault::budget) fault_code = "  if f == " + std::to_string(fault_frame) + " then while true do end end\n";
  if (fault == Fault::error) fault_code = "  if f == " + std::to_string(fault_frame) + " then error(\"boom\") end\n";
  return "local n = 0\n"
         "function frame_begin(f)\n  n = n + 1\n  sega.mem.write(\"m68k.work_ram\", 0x100, 4, n)\n" + fault_code + "end\n"
         "function input(f)\n  local p = sega.input.get(1)\n  if p % 2 == 0 then p = p + 1 end\n  sega.input.set(1, p)\nend\n"
         "function post_input(f)\n  sega.mem.write(\"m68k.work_ram\", 0x110, 2, sega.input.get(1))\nend\n"
         "function frame_end(f)\n  local c = sega.mem.read(\"m68k.work_ram\", 0x120, 1)\n  sega.mem.write(\"m68k.work_ram\", 0x121, 1, (c + 1) % 256)\nend\n"
         "function save() return { n = n } end\n"
         "function load(v) n = v.n end\n";
}

struct LuauMod {
  mod::Package package;
  std::unique_ptr<luau::PhaseProvider> provider;
  std::string error;
};
LuauMod make_luau(const std::string &source) {
  LuauMod m;
  mod::Manifest man;
  man.id = "replay";
  man.version = "1.0.0";
  man.name = "Replay";
  man.platform = "genesis";
  man.source_kind = "cartridge-image";
  man.revisions.push_back({"rev-a", std::string(64, 'a'), ""});
  std::vector<std::string> caps = kLuauCaps;
  std::sort(caps.begin(), caps.end());
  man.capabilities = caps;
  man.state_schema = 2;
  mod::Payload p;
  p.kind = "luau-source";
  p.path = "scripts/main.luau";
  p.size = source.size();
  const std::vector<std::uint8_t> bytes(source.begin(), source.end());
  p.sha256 = sha256_hex(bytes);
  p.role = "script";
  man.payloads.push_back(p);
  m.package = mod::read_package(mod::pack_package(man, {{"scripts/main.luau", bytes}}));
  if (!m.package.ok) { std::printf("package rejected: %s\n", m.package.rejection.message.c_str()); std::exit(2); }
  const luau::CompileResult c = luau::compile_mod(m.package);
  if (!c.ok) { std::printf("compile failed: %s\n", c.rejection.message.c_str()); std::exit(2); }
  luau::Limits limits;
  limits.callback_units = 20000;      // a runaway loop faults on the deterministic budget quickly
  limits.callback_watchdog_ms = 0;    // wall-clock watchdogs are off: a host-timing fault would make the run non-replayable (phases section 7)
  limits.frame_watchdog_ms = 0;
  luau::HostConfig host;
  host.capabilities = kLuauCaps;
  m.provider = luau::PhaseProvider::create(m.package.identity, c.compiled, limits, host, genesis_mod_fault_current, m.error);
  return m;
}

std::string hex(const uint8_t *d, std::size_t n) {
  static const char *h = "0123456789abcdef";
  std::string s;
  for (std::size_t i = 0; i < n; ++i) { s += h[d[i] >> 4U]; s += h[d[i] & 15U]; }
  return s;
}

struct Spec {
  bool luau = false;
  Fault fault = Fault::none;
  uint64_t fault_frame = 0;
  uint8_t native_identity = 0x61;
  uint32_t native_schema = 2;
  uint8_t region = 0;
  uint8_t source = 0x44;
  bool extra_capability = false;   // a different declared capability set (another build of "the same" mod)
};

struct Session {
  std::unique_ptr<GenesisRuntime> rt = std::make_unique<GenesisRuntime>();
  std::unique_ptr<GenesisModScheduler> sched = std::make_unique<GenesisModScheduler>();
  NativeMod native;
  SegaModProvider native_provider{};
  LuauMod lm;
  std::vector<std::string> lines;
  GenesisSnapshotModState snap{};
  GenesisSnapshotMachine machine{};
  GenesisSnapshotIdentity id{};
  bool attached = false;
  std::string rejection;
  static void sink(void *ctx, const char *line, uint32_t len) { static_cast<Session *>(ctx)->lines.emplace_back(line, len); }

  explicit Session(const Spec &spec) {
    setup(*rt);
    rt->region_profile = spec.region;
    GenesisModConfig c{};
    c.mod_id = "replay";
    c.required[0] = kCaps | (spec.extra_capability ? cap(SEGA_MOD_CAP_HOST_LOG) : 0U);
    c.phase_mask = kPhases;
    c.ranges = kRanges;
    c.range_count = 1U;
    c.state_schema = spec.native_schema;
    c.log_sink = sink;
    c.log_ctx = this;
    if (spec.luau) {
      lm = make_luau(luau_source(spec.fault_frame, spec.fault));
      if (!lm.provider) { check(false, "the Luau mod loads: " + lm.error); return; }
      c.provider = lm.provider->provider();
      c.provider_state = lm.provider->state();
      for (unsigned i = 0; i < 32U; ++i) c.identity_sha256[i] = static_cast<uint8_t>(std::strtoul(lm.package.identity.substr(i * 2U, 2U).c_str(), nullptr, 16));
    } else {
      native.fault = spec.fault;
      native.fault_frame = spec.fault_frame;
      native_provider.size = sizeof native_provider;
      native_provider.abi_major = SEGA_MOD_ABI_MAJOR;
      native_provider.platform = SEGA_MOD_PLATFORM_GENESIS;
      native_provider.name = "replay-native";
      native_provider.implementation_version = 1;
      native_provider.phase = native_phase;
      native_provider.save_state = native_save;
      native_provider.load_state = native_load;
      c.provider = &native_provider;
      c.provider_state = &native;
      std::memset(c.identity_sha256, spec.native_identity, 32U);
    }
    const char *why = "";
    attached = genesis_mod_scheduler_attach(sched.get(), rt.get(), &c, &why) == 0;
    rejection = why;
    check(attached, std::string("the replay mod attaches: ") + why);
    genesis_mod_snapshot_state(sched.get(), &snap);
    machine.runtime = rt.get();
    machine.mod = &snap;
    std::memset(id.source_sha256, spec.source, 32U);
  }
  ~Session() { genesis_mod_scheduler_detach(sched.get()); }

  std::string hash() {
    GenesisSnapshotBuffer b{nullptr, 0};
    if (genesis_snapshot_capture(&machine, &id, nullptr, &b) != GENESIS_SNAPSHOT_OK) return "capture-failed";
    const std::string h = hex(b.data + b.size - 32U, 32U);
    genesis_snapshot_buffer_free(&b);
    return h;
  }
  // Runs the recorded input up to `frame`, appending the state hash each time a frame boundary was crossed.
  void run_to(uint64_t frame, std::vector<std::string> *hashes) {
    uint64_t last = rt->virtual_frame_count;
    for (uint32_t guard = 0; rt->virtual_frame_count < frame; ++guard) {
      if (guard > 200000U) { check(false, "the replay did not advance"); return; }
      rt->pad1 = recorded_pad(rt->virtual_frame_count);
      if (genesis_runtime_run(rt.get(), synthetic_dispatch, 997U).kind != GENESIS_RUNNER_RESOURCE_LIMIT) { check(false, "the synthetic program stopped"); return; }
      if (rt->virtual_frame_count != last) {
        last = rt->virtual_frame_count;
        if (hashes != nullptr) hashes->push_back(hash());
      }
    }
  }
  uint32_t mod_counter() const { return native.n; }
  uint32_t ram_u32(uint32_t off) const { return (static_cast<uint32_t>(rt->work_ram[off]) << 24U) | (static_cast<uint32_t>(rt->work_ram[off + 1U]) << 16U) | (static_cast<uint32_t>(rt->work_ram[off + 2U]) << 8U) | static_cast<uint32_t>(rt->work_ram[off + 3U]); }
  uint64_t write_digest_total() const { return sched->write_total; }
  std::string write_digest() const { uint8_t d[32]; genesis_mod_write_log_digest(sched.get(), d); return hex(d, 32); }
};

struct Shot {
  GenesisSnapshotBuffer buf{nullptr, 0};
  ~Shot() { genesis_snapshot_buffer_free(&buf); }
  bool take(Session &s) { return genesis_snapshot_capture(&s.machine, &s.id, nullptr, &buf) == GENESIS_SNAPSHOT_OK; }
};

const char *kind_name(const Spec &s) { return s.luau ? "Luau" : "native"; }

// 1. Replay equivalence: two runs from reset with the same recorded input have identical per-frame hashes; different input and the unmodded program differ.
void replay_equivalence() {
  for (const bool luau : {false, true}) {
    Spec spec;
    spec.luau = luau;
    std::vector<std::string> h1, h2;
    std::string w1, w2, lines1, lines2;
    {
      Session a(spec);
      a.run_to(40U, &h1);
      w1 = a.write_digest();
      check(a.sched->fault.faulted == 0U && a.sched->write_total > 100U, std::string(kind_name(spec)) + ": the mod ran and committed writes without fault (" + std::to_string(a.sched->write_total) + ")");
    }
    {
      Session b(spec);
      b.run_to(40U, &h2);
      w2 = b.write_digest();
    }
    check(h1.size() >= 38U && h1 == h2, std::string(kind_name(spec)) + ": two replays of the same recorded input have identical state hashes for all " + std::to_string(h1.size()) + " frames");
    check(w1 == w2, std::string(kind_name(spec)) + ": the committed-write witness is identical");
    // control: a different recording changes the hashes (the hash is sensitive, and the mod really interposes on input)
    Session c(spec);
    std::vector<std::string> h3;
    for (uint32_t guard = 0; c.rt->virtual_frame_count < 40U && guard < 200000U; ++guard) {
      c.rt->pad1 = static_cast<uint8_t>(recorded_pad(c.rt->virtual_frame_count) ^ 0x10U);
      (void)genesis_runtime_run(c.rt.get(), synthetic_dispatch, 997U);
    }
    check(c.hash() != h1.back(), std::string(kind_name(spec)) + ": a different recording gives different state");
    // the mod's input interception was seen by the guest: bit 0 was forced on, so the guest never saw a pad with bit 0 clear after the first frame
    Session d(spec);
    d.run_to(10U, nullptr);
    check((d.rt->d[1] & 1U) != 0U || d.rt->mod_pad_active == 1U, std::string(kind_name(spec)) + ": the guest reads the intercepted pad");
  }
  // the Luau and native twins implement the same behavior: same guest-visible memory after the same replay
  Spec n, l;
  l.luau = true;
  Session a(n), b(l);
  a.run_to(25U, nullptr);
  b.run_to(25U, nullptr);
  check(a.write_digest() == b.write_digest() && std::memcmp(a.rt->work_ram, b.rt->work_ram, 65536U) == 0 && a.rt->d[2] == b.rt->d[2], "native and Luau twins leave identical guest state and write witness");
}

// 2. Restore equivalence under replay: save mid-run, run on, restore, replay the recording from there: identical hashes.
void restore_equivalence() {
  for (const bool luau : {false, true}) {
    Spec spec;
    spec.luau = luau;
    Session a(spec);
    std::vector<std::string> head, tail;
    a.run_to(17U, &head);
    Shot s;
    check(s.take(a), std::string(kind_name(spec)) + ": capture at frame 17");
    const std::string at_save = a.hash();
    a.run_to(40U, &tail);
    check(genesis_snapshot_restore(&a.machine, &a.id, s.buf.data, s.buf.size) == GENESIS_SNAPSHOT_OK, std::string(kind_name(spec)) + ": restore");
    check(a.hash() == at_save, std::string(kind_name(spec)) + ": identical state hash right after the restore");
    std::vector<std::string> again;
    a.run_to(40U, &again);
    check(!tail.empty() && again == tail, std::string(kind_name(spec)) + ": replaying the recording after a restore gives the identical hash sequence (" + std::to_string(again.size()) + " frames)");
    // and into a brand-new process (fresh session)
    Session fresh(spec);
    check(genesis_snapshot_restore(&fresh.machine, &fresh.id, s.buf.data, s.buf.size) == GENESIS_SNAPSHOT_OK, std::string(kind_name(spec)) + ": restore into a fresh session");
    std::vector<std::string> from_fresh;
    fresh.run_to(40U, &from_fresh);
    check(from_fresh == tail && fresh.write_digest() == a.write_digest(), std::string(kind_name(spec)) + ": a fresh session continues identically (hashes and write witness)");
  }
}

// 3. Sandbox failure during replay matches the contract (phases section 6.1): the callback's staged effects are discarded, the mod is disabled, the stable diagnostic is surfaced,
//    the guest continues; the whole thing is reproducible, including across a restore.
void sandbox_failure_during_replay() {
  struct Case { const char *what; Spec spec; const char *cause; };
  std::vector<Case> cases;
  { Spec s; s.fault = Fault::range; s.fault_frame = 12; cases.push_back({"native range violation", s, "range"}); }
  { Spec s; s.luau = true; s.fault = Fault::budget; s.fault_frame = 12; cases.push_back({"Luau budget exhaustion", s, "budget"}); }
  { Spec s; s.luau = true; s.fault = Fault::error; s.fault_frame = 12; cases.push_back({"Luau script error", s, "error"}); }
  for (const Case &c : cases) {
    std::vector<std::string> h1, h2;
    std::string text1, text2;
    uint32_t ram_100 = 0, ram_after = 0;
    uint64_t callbacks = 0;
    {
      Session a(c.spec);
      a.run_to(11U, &h1);
      const uint32_t before = a.ram_u32(0x100);
      a.run_to(40U, &h1);
      text1 = a.sched->fault.text;
      ram_100 = before;
      ram_after = a.ram_u32(0x100);
      callbacks = a.sched->callbacks_run;
      check(a.sched->disabled == 1U && a.sched->fault.faulted == 1U && std::string(a.sched->fault.cause) == c.cause && a.sched->fault.frame == 12U && a.sched->fault.phase == SEGA_MOD_PHASE_FRAME_BEGIN,
            std::string(c.what) + ": the mod is disabled with cause " + c.cause + " at frame 12, phase frame-begin (" + text1 + ")");
      check(text1 == std::string("E_MOD_FAULT cause=") + c.cause + " mod=replay phase=frame-begin frame=12", std::string(c.what) + ": the diagnostic is the stable class: " + text1);
      check(!a.lines.empty() && a.lines.back() == text1, std::string(c.what) + ": the diagnostic reached the host log sink");
      check(ram_after == ram_100, std::string(c.what) + ": the faulting callback's staged write never committed (RAM keeps the previous frame's value)");
      check(a.rt->mod_pad_active == 0U, std::string(c.what) + ": pad interception ended; the guest reads the physical pad");
      check(c.spec.luau ? !a.lm.provider->nondeterministic() : true, std::string(c.what) + ": the fault is deterministic (no watchdog involved)");
      const uint64_t total_at_end = a.sched->write_total;
      a.run_to(48U, nullptr);
      check(a.sched->write_total == total_at_end && a.sched->callbacks_run == callbacks, std::string(c.what) + ": a disabled mod is inert for the rest of the replay");
    }
    {
      Session b(c.spec);
      b.run_to(40U, &h2);
      text2 = b.sched->fault.text;
    }
    check(h1.size() >= 38U && h1 == h2 && text1 == text2, std::string(c.what) + ": the faulting replay is bit-identical across runs (hashes and diagnostic)");
    // replay across a restore: a state saved BEFORE the fault, restored into a fresh session, faults again at the same frame with the same diagnostic and the same hashes
    {
      Session a(c.spec);
      a.run_to(8U, nullptr);
      Shot s;
      check(s.take(a), std::string(c.what) + ": capture before the fault");
      Session fresh(c.spec);
      check(genesis_snapshot_restore(&fresh.machine, &fresh.id, s.buf.data, s.buf.size) == GENESIS_SNAPSHOT_OK, std::string(c.what) + ": restore the pre-fault state into a fresh session");
      std::vector<std::string> h3;
      fresh.run_to(40U, &h3);
      check(std::string(fresh.sched->fault.text) == text1 && h3.size() == 40U - 8U && std::equal(h3.begin(), h3.end(), h1.end() - static_cast<std::ptrdiff_t>(h3.size())), std::string(c.what) + ": the fault replays identically from a restored state");
      // the state saved AFTER the fault restores as disabled and inert
      Shot after;
      check(after.take(fresh), std::string(c.what) + ": a disabled mod's state captures");
      Session another(c.spec);
      check(genesis_snapshot_restore(&another.machine, &another.id, after.buf.data, after.buf.size) == GENESIS_SNAPSHOT_OK && another.sched->disabled == 1U && text1 == another.sched->fault.text,
            std::string(c.what) + ": the post-fault state restores as disabled with the same diagnostic");
      const uint64_t callbacks_before = another.sched->callbacks_run;
      another.run_to(another.rt->virtual_frame_count + 6U, nullptr);
      check(another.sched->callbacks_run == callbacks_before, std::string(c.what) + ": and stays inert");
      // restoring an ENABLED state into the faulted session: native mods can resume; a Luau VM never recovers from a fault, so the restore is refused and nothing of the guest changes
      std::vector<uint8_t> ram(fresh.rt->work_ram, fresh.rt->work_ram + 65536U);
      const uint64_t frame = fresh.rt->virtual_frame_count;
      const GenesisSnapshotStatus st = genesis_snapshot_restore(&fresh.machine, &fresh.id, s.buf.data, s.buf.size);
      if (c.spec.luau) {
        check(st == GENESIS_SNAPSHOT_MOD_REJECTED && std::memcmp(ram.data(), fresh.rt->work_ram, 65536U) == 0 && fresh.rt->virtual_frame_count == frame,
              std::string(c.what) + ": a faulted Luau VM cannot take an enabled state back: E_STATE_MOD_REJECTED and the guest is untouched");
      } else {
        check(st == GENESIS_SNAPSHOT_OK && fresh.sched->disabled == 0U, std::string(c.what) + ": a native mod resumes from a pre-fault state");
      }
    }
  }
}

// 4. Incompatible state is rejected and never partially applied.
void incompatible_state() {
  Spec spec;
  Session a(spec);
  a.run_to(10U, nullptr);
  Shot good;
  check(good.take(a), "capture a good state");
  a.run_to(14U, nullptr);   // the live machine is somewhere else: any partial application would be visible
  const std::string live = a.hash();
  const uint32_t loads = a.native.loads;

  struct Other { const char *what; Spec spec; GenesisSnapshotStatus want; };
  std::vector<Other> others;
  { Spec s; s.native_identity = 0x62; others.push_back({"another mod identity", s, GENESIS_SNAPSHOT_MOD_MISMATCH}); }
  { Spec s; s.native_schema = 3; others.push_back({"another state schema", s, GENESIS_SNAPSHOT_MOD_MISMATCH}); }
  { Spec s; s.extra_capability = true; others.push_back({"another capability set", s, GENESIS_SNAPSHOT_MOD_MISMATCH}); }
  { Spec s; s.region = 1; others.push_back({"another region", s, GENESIS_SNAPSHOT_INCOMPATIBLE}); }
  { Spec s; s.source = 0x45; others.push_back({"another game image", s, GENESIS_SNAPSHOT_WRONG_CARTRIDGE}); }
  for (const Other &o : others) {
    Session b(o.spec);
    b.run_to(14U, nullptr);
    const std::string hb = b.hash();
    const GenesisSnapshotStatus st = genesis_snapshot_restore(&b.machine, &b.id, good.buf.data, good.buf.size);
    check(st == o.want, std::string("restore with ") + o.what + ": expected status " + std::to_string(o.want) + ", got " + std::to_string(st));
    check(b.hash() == hb && b.native.loads == 0U, std::string("nothing is applied (guest and mod untouched, mod load never called): ") + o.what);
  }
  // a Luau mod does not accept a native mod's state (another identity) and vice versa
  {
    Spec l;
    l.luau = true;
    Session b(l);
    b.run_to(14U, nullptr);
    const std::string hb = b.hash();
    check(genesis_snapshot_restore(&b.machine, &b.id, good.buf.data, good.buf.size) == GENESIS_SNAPSHOT_MOD_MISMATCH && b.hash() == hb, "a Luau mod refuses a native mod's state and stays unchanged");
  }
  // truncation, unsealed corruption and sealed-but-crafted MODSTATE bodies
  const std::vector<uint8_t> bytes(good.buf.data, good.buf.data + good.buf.size);
  uint32_t rejected = 0;
  for (std::size_t cut = 0; cut < bytes.size(); cut += std::max<std::size_t>(1U, bytes.size() / 61U)) {
    const std::vector<uint8_t> t(bytes.begin(), bytes.begin() + static_cast<long>(cut));
    if (genesis_snapshot_restore(&a.machine, &a.id, t.data(), t.size()) == GENESIS_SNAPSHOT_OK) check(false, "a truncated state was accepted at " + std::to_string(cut));
    else ++rejected;
  }
  check(a.hash() == live && a.native.loads == loads, "truncated states change nothing (" + std::to_string(rejected) + " cuts)");
  rejected = 0;
  for (std::size_t at = 0; at < bytes.size(); at += std::max<std::size_t>(1U, bytes.size() / 83U)) {
    std::vector<uint8_t> t = bytes;
    t[at] ^= 0x5AU;
    if (genesis_snapshot_restore(&a.machine, &a.id, t.data(), t.size()) == GENESIS_SNAPSHOT_OK) check(false, "a corrupted state was accepted at " + std::to_string(at));
    else ++rejected;
  }
  check(a.hash() == live && a.native.loads == loads && rejected > 60U, "corrupted states change nothing (" + std::to_string(rejected) + " flips)");
  {   // sealed edits of every MODSTATE byte: either rejected with the machine untouched, or accepted as a (self-consistent) state that the next good restore fully replaces
    const uint32_t count = rd_le(bytes.data() + 28, 2U);
    uint32_t off = 0, len = 0;
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t *e = bytes.data() + 156U + i * 12U;
      if (rd_le(e, 2U) == 11U) { off = rd_le(e + 4, 4U); len = rd_le(e + 8, 4U); }
    }
    check(len > 76U, "MODSTATE found");
    const std::string saved_hash = [&] { Session t(spec); t.run_to(10U, nullptr); return t.hash(); }();
    uint32_t refused = 0, accepted = 0;
    for (uint32_t k = 0; k < len; ++k) {
      std::vector<uint8_t> t = bytes;
      t[off + k] ^= 0x01U << (k % 8U);
      GenesisSha256 sha;
      genesis_sha256_init(&sha);
      genesis_sha256_update(&sha, t.data(), static_cast<uint32_t>(t.size() - 32U));
      genesis_sha256_final(&sha, t.data() + t.size() - 32U);
      const std::string before = a.hash();
      const GenesisSnapshotStatus st = genesis_snapshot_restore(&a.machine, &a.id, t.data(), t.size());
      if (st == GENESIS_SNAPSHOT_OK) {
        ++accepted;
        check(genesis_snapshot_restore(&a.machine, &a.id, good.buf.data, good.buf.size) == GENESIS_SNAPSHOT_OK && a.hash() == saved_hash, "a good restore replaces whatever an accepted crafted state left (byte " + std::to_string(k) + ")");
      } else {
        ++refused;
        check(st == GENESIS_SNAPSHOT_MOD_MISMATCH || st == GENESIS_SNAPSHOT_MOD_REJECTED || st == GENESIS_SNAPSHOT_UNKNOWN_SECTION || st == GENESIS_SNAPSHOT_BAD_LAYOUT, "a crafted MODSTATE byte " + std::to_string(k) + " fails with a stable class (got " + std::to_string(st) + ")");
        if (a.hash() != before) { check(false, "a refused crafted state changed the machine (byte " + std::to_string(k) + ")"); break; }
      }
    }
    check(refused > 40U && accepted + refused == len, "crafted MODSTATE bytes: " + std::to_string(refused) + " refused, " + std::to_string(accepted) + " accepted-and-replaceable");
    check(genesis_snapshot_restore(&a.machine, &a.id, good.buf.data, good.buf.size) == GENESIS_SNAPSHOT_OK && a.hash() == saved_hash, "the good state still restores exactly after all of it");
  }
}

}  // namespace

int main() {
  replay_equivalence();
  restore_equivalence();
  sandbox_failure_during_replay();
  incompatible_state();
  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("genesis_mod_replay_tests: ok\n");
  return 0;
}
