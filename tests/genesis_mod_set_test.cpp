// SEG-141-T003 (ADR 0327): several mods on one Genesis runtime. Dispatch order, the input chain, failure isolation by fault injection in every phase, the combined state, exclusive replace-hook ownership,
// shared hook order and the measured dispatch overhead. Project-authored synthetic program and native ABI 2 providers; no ROM, no host clock reaches the guest.
#include "mod_scheduler.h"
#include "runtime.h"

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

// The guest: reads its pad on every dispatch (d1 = last byte, d2 = running sum) and counts in one work-RAM byte.
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
    if (genesis_runtime_run(&r, synthetic_dispatch, 997U).kind != GENESIS_RUNNER_RESOURCE_LIMIT) { check(false, "the synthetic program stopped unexpectedly"); return; }
  }
}

std::string hex(const uint8_t *p, std::size_t n) {
  static const char *d = "0123456789abcdef";
  std::string s;
  for (std::size_t i = 0; i < n; ++i) { s += d[p[i] >> 4U]; s += d[p[i] & 15U]; }
  return s;
}
std::string guest_digest(const GenesisRuntime &r) {
  GenesisSha256 sha;
  uint8_t out[32];
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, r.work_ram, sizeof(r.work_ram));
  genesis_sha256_update(&sha, reinterpret_cast<const uint8_t *>(r.d), sizeof(r.d));
  genesis_sha256_update(&sha, reinterpret_cast<const uint8_t *>(&r.scheduler.master_ticks), sizeof(r.scheduler.master_ticks));
  genesis_sha256_final(&sha, out);
  return hex(out, 32);
}
std::string witness(const GenesisModScheduler &s) {
  uint8_t out[32];
  genesis_mod_write_log_digest(&s, out);
  return hex(out, 32);
}

constexpr uint32_t bit(uint32_t phase) { return 1U << phase; }
constexpr uint32_t kAllPhases = bit(SEGA_MOD_PHASE_RESET) | bit(SEGA_MOD_PHASE_FRAME_BEGIN) | bit(SEGA_MOD_PHASE_INPUT) | bit(SEGA_MOD_PHASE_POST_INPUT) | bit(SEGA_MOD_PHASE_FRAME_END) | bit(SEGA_MOD_PHASE_RENDER);
constexpr uint64_t cap(unsigned id) { return UINT64_C(1) << id; }
constexpr uint64_t kCaps = cap(SEGA_MOD_CAP_MEMORY_READ) | cap(SEGA_MOD_CAP_MEMORY_WRITE) | cap(SEGA_MOD_CAP_INPUT_READ) | cap(SEGA_MOD_CAP_INPUT_INTERCEPT) | cap(SEGA_MOD_CAP_STATE_MOD);
const GenesisModRange kRanges[] = {{SEGA_GEN_SPACE_M68K_WORK_RAM, 0x100U, 0x100U, GENESIS_MOD_ACCESS_READ | GENESIS_MOD_ACCESS_WRITE}};

// One mod. Its guest effects: at frame-end it appends its digit to the SHARED word at 0x100 (word = word * 10 + digit), so the final value encodes the dispatch order; in `input` it applies
// `pad = (pad * mul + digit) & 0xFF` to port 1, so the chain order is visible in what the guest reads. Its private counter is its saved state.
struct Mod {
  uint32_t digit = 1, mul = 3;
  int fault_phase = 0;                  // inject: read outside the declared ranges in this phase
  bool quiet = false;                   // no guest effect at all (used for the overhead measurement and as a pure faulter)
  bool refuse_load = false;
  uint32_t counter = 0, saw_post_pad = 0, saw_frame_begin_pad = 0;
  std::vector<std::string> *order = nullptr;   // shared call log (hooks)
  int replace = 0;                      // the hook result
};

int mod_init(void *state, const SegaModHost *) { static_cast<Mod *>(state)->counter = 0; return 0; }
void mod_phase(void *state, const SegaModHost *host) {
  Mod &m = *static_cast<Mod *>(state);
  uint32_t v = 0;
  if (m.fault_phase == static_cast<int>(host->phase)) {
    if (host->phase == SEGA_MOD_PHASE_INPUT) (void)host->input_set(1U, 0xFFU);   // staged, then discarded by the fault below
    (void)host->read(SEGA_GEN_SPACE_M68K_WORK_RAM, 0x9000U, 2U, &v);
    return;
  }
  if (m.quiet) return;
  if (host->phase == SEGA_MOD_PHASE_INPUT) {
    (void)host->input_get(1U, &v);
    (void)host->input_set(1U, (v * m.mul + m.digit) & 0xFFU);
  } else if (host->phase == SEGA_MOD_PHASE_POST_INPUT) {
    (void)host->input_get(1U, &m.saw_post_pad);
  } else if (host->phase == SEGA_MOD_PHASE_FRAME_BEGIN) {
    (void)host->input_get(1U, &m.saw_frame_begin_pad);
  } else if (host->phase == SEGA_MOD_PHASE_FRAME_END) {
    (void)host->read(SEGA_GEN_SPACE_M68K_WORK_RAM, 0x100U, 2U, &v);
    (void)host->write(SEGA_GEN_SPACE_M68K_WORK_RAM, 0x100U, 2U, (v * 10U + m.digit) & 0xFFFFU);
    ++m.counter;
  }
}
int mod_hook(void *state, const SegaModHost *host, uint32_t) {
  Mod &m = *static_cast<Mod *>(state);
  if (m.order != nullptr) m.order->push_back(std::to_string(m.digit) + (host->phase == SEGA_MOD_PHASE_HOOK_ENTRY ? "e" : "x"));
  if (m.fault_phase == static_cast<int>(host->phase)) {
    uint32_t v = 0;
    (void)host->read(SEGA_GEN_SPACE_M68K_WORK_RAM, 0x9000U, 2U, &v);
  }
  return m.replace;
}
int mod_save(void *state, uint8_t *out, uint32_t cap_bytes, uint32_t *len) {
  if (cap_bytes < 4U) return 1;
  std::memcpy(out, &static_cast<Mod *>(state)->counter, 4U);
  *len = 4U;
  return 0;
}
int mod_load(void *state, const uint8_t *data, uint32_t len) {
  Mod &m = *static_cast<Mod *>(state);
  if (len != 4U) return 1;
  if (m.refuse_load) { m.refuse_load = false; return 1; }   // refuses the offered state once, then takes its previous one back
  std::memcpy(&m.counter, data, 4U);
  return 0;
}

SegaModProvider make_provider() {
  SegaModProvider p;
  std::memset(&p, 0, sizeof(p));
  p.size = sizeof(SegaModProvider);
  p.abi_major = SEGA_MOD_ABI_MAJOR;
  p.platform = SEGA_MOD_PLATFORM_GENESIS;
  p.name = "set-member";
  p.implementation_version = 1;
  p.init = mod_init;
  p.phase = mod_phase;
  p.hook = mod_hook;
  p.save_state = mod_save;
  p.load_state = mod_load;
  return p;
}

struct Rig {
  std::unique_ptr<GenesisRuntime> rt_heap{new GenesisRuntime};
  GenesisRuntime &rt = *rt_heap;
  std::vector<std::unique_ptr<GenesisModScheduler>> schedulers;
  std::vector<GenesisModScheduler *> pointers;
  std::vector<std::unique_ptr<Mod>> mods;
  std::vector<std::string> ids;
  SegaModProvider provider = make_provider();
  std::unique_ptr<GenesisModSet> set{new GenesisModSet};
  const char *rejection = "";
  uint32_t bad_member = 0;
  int result = -1;
  // `digits` are the members in composite order. `identity_byte` stands for the composite identity.
  void attach(const std::vector<Mod> &spec, uint8_t identity_byte = 7, uint32_t mask = kAllPhases, const std::vector<GenesisHookSiteInfo> *sites = nullptr, const std::vector<std::vector<GenesisModHookBinding>> *bindings = nullptr) {
    setup(rt);
    rt.pad1 = 0x05U;
    std::vector<GenesisModConfig> configs;
    ids.clear();
    ids.reserve(spec.size());
    for (std::size_t i = 0; i < spec.size(); ++i) {
      mods.push_back(std::make_unique<Mod>(spec[i]));
      schedulers.push_back(std::make_unique<GenesisModScheduler>());
      pointers.push_back(schedulers.back().get());
      ids.push_back("mod-" + std::to_string(spec[i].digit));
      GenesisModConfig c{};
      c.mod_id = ids.back().c_str();
      c.provider = &provider;
      c.provider_state = mods.back().get();
      c.phase_mask = mask;
      c.required[0] = kCaps;
      c.ranges = kRanges;
      c.range_count = 1U;
      c.state_schema = 1U;
      c.identity_sha256[0] = static_cast<uint8_t>(spec[i].digit);
      if (sites != nullptr && !(*bindings)[i].empty()) {
        c.required[0] |= cap(SEGA_MOD_CAP_HOOK_FUNCTION);
        c.phase_mask |= bit(SEGA_MOD_PHASE_HOOK_ENTRY) | bit(SEGA_MOD_PHASE_HOOK_EXIT);
        c.hooks = (*bindings)[i].data();
        c.hook_count = static_cast<uint32_t>((*bindings)[i].size());
        c.sites = sites->data();
        c.site_count = static_cast<uint32_t>(sites->size());
      }
      configs.push_back(c);
    }
    uint8_t identity[32] = {0};
    identity[0] = identity_byte;
    result = genesis_mod_set_attach(set.get(), &rt, pointers.data(), configs.data(), static_cast<uint32_t>(configs.size()), identity, &rejection, &bad_member);
  }
  uint32_t word() const { return (static_cast<uint32_t>(rt.work_ram[0x100]) << 8U) | rt.work_ram[0x101]; }
};

Mod member(uint32_t digit, uint32_t mul = 3) {
  Mod m;
  m.digit = digit;
  m.mul = mul;
  return m;
}

void ordering_permutations() {
  std::array<uint32_t, 3> order = {1, 2, 3};
  const std::array<uint32_t, 4> muls = {0, 3, 5, 7};
  std::vector<std::string> digests;
  do {
    std::vector<Mod> spec;
    for (uint32_t d : order) spec.push_back(member(d, muls[d]));
    Rig a, b;
    a.attach(spec);
    b.attach(spec);
    check(a.result == 0 && b.result == 0, "attach a three-mod set");
    run_frames(a.rt, 4U);
    run_frames(b.rt, 4U);
    uint32_t expected_word = 0, expected_pad = 0x05U;   // hand-derived: the fold in composite order
    for (int frame = 0; frame < 4; ++frame)
      for (uint32_t d : order) expected_word = (expected_word * 10U + d) & 0xFFFFU;
    for (uint32_t d : order) expected_pad = (expected_pad * muls[d] + d) & 0xFFU;
    const std::string name = std::to_string(order[0]) + std::to_string(order[1]) + std::to_string(order[2]);
    check(a.word() == expected_word, "frame-end runs in composite order " + name + ": " + std::to_string(a.word()) + " vs " + std::to_string(expected_word));
    check(a.rt.mod_pad_active == 1U && a.rt.mod_pad1 == expected_pad, "the input chain runs in composite order " + name);
    for (const auto &m : a.mods) check(m->saw_post_pad == expected_pad && m->saw_frame_begin_pad == expected_pad, "every member sees the composed pad " + name);
    check(guest_digest(a.rt) == guest_digest(b.rt), "the same set replays identically " + name);
    for (std::size_t i = 0; i < 3; ++i) check(witness(*a.schedulers[i]) == witness(*b.schedulers[i]), "per-member witnesses replay " + name);
    digests.push_back(guest_digest(a.rt));
  } while (std::next_permutation(order.begin(), order.end()));
  std::sort(digests.begin(), digests.end());
  check(digests.size() == 6 && std::unique(digests.begin(), digests.end()) == digests.end(), "each of the six orders is a different, defined result");
}

// A member that faults in its FIRST callback of `phase` and otherwise does nothing: the others must behave exactly as in the set without it.
void failure_isolation() {
  Rig reference;
  reference.attach({member(1, 3), member(3, 7)});
  run_frames(reference.rt, 4U);
  for (uint32_t phase : {SEGA_MOD_PHASE_RESET, SEGA_MOD_PHASE_FRAME_BEGIN, SEGA_MOD_PHASE_INPUT, SEGA_MOD_PHASE_POST_INPUT, SEGA_MOD_PHASE_FRAME_END, SEGA_MOD_PHASE_RENDER}) {
    Mod bad = member(2, 5);
    bad.quiet = true;
    bad.fault_phase = static_cast<int>(phase);
    Rig r;
    r.attach({member(1, 3), bad, member(3, 7)});
    run_frames(r.rt, 4U);
    const std::string name = genesis_mod_phase_name(phase);
    const std::string cause = phase == SEGA_MOD_PHASE_RENDER ? "capability" : "range";   // a render read needs frame.work_ram, which this mod does not hold
    check(r.schedulers[1]->disabled && r.schedulers[1]->fault.cause != nullptr && r.schedulers[1]->fault.cause == cause && r.schedulers[1]->fault.phase == phase, "the injected fault disables the faulting mod: " + name);
    check(!r.schedulers[0]->disabled && !r.schedulers[2]->disabled, "no other mod is disabled: " + name);
    check(guest_digest(r.rt) == guest_digest(reference.rt), "the guest equals the set without the faulted mod: " + name);
    check(witness(*r.schedulers[0]) == witness(*reference.schedulers[0]) && witness(*r.schedulers[2]) == witness(*reference.schedulers[1]), "the other mods' writes are unchanged: " + name);
    check(r.rt.mod_pad_active == 1U && r.rt.mod_pad1 == reference.rt.mod_pad1, "the pad latch of the live mods survives: " + name);
  }
  // the last input mod faulting releases the latch: the guest reads the physical pad again
  Mod only = member(1, 3);
  only.fault_phase = SEGA_MOD_PHASE_POST_INPUT;
  Rig r;
  r.attach({only});
  run_frames(r.rt, 2U);
  check(r.schedulers[0]->disabled && r.rt.mod_pad_active == 0U && r.rt.d[1] != 0U, "no live input mod: the latch is released");
}

void attach_refusals() {
  Rig dup;
  dup.attach({member(1), member(1)});
  check(dup.result == 1 && std::string(dup.rejection) == "E_SET_DUPLICATE_MOD" && dup.bad_member == 1U && dup.rt.frame_hooks == nullptr, "one mod id twice is refused and nothing stays attached");
  Rig zero;
  zero.attach({member(1)}, 0);
  check(zero.result == 1 && std::string(zero.rejection) == "E_SET_IDENTITY" && zero.rt.frame_hooks == nullptr, "a set needs its composite identity");
  Rig capability;
  capability.attach({member(1), member(2)}, 7, bit(SEGA_MOD_PHASE_HOOK_ENTRY));
  check(capability.result == 1 && capability.bad_member == 0U && capability.rt.frame_hooks == nullptr && capability.rt.function_hooks == nullptr, "a member's own refusal refuses the whole set");
}

void hooks() {
  const std::vector<GenesisHookSiteInfo> sites = {{0x300U, "watch", GENESIS_HOOK_MODE_BEFORE | GENESIS_HOOK_MODE_AFTER, 0U, 0U, 0U, 0U, 0U}, {0x400U, "swap", GENESIS_HOOK_MODE_REPLACE, 0U, 0U, 8U, 0U, 0U}};
  for (const bool reversed : {false, true}) {
    std::vector<std::string> log;
    Mod a = member(reversed ? 2 : 1), b = member(reversed ? 1 : 2);
    a.order = b.order = &log;
    const std::vector<std::vector<GenesisModHookBinding>> bind = {{{"watch", 1U}}, {{"watch", 9U}}};
    Rig r;
    r.attach({a, b}, 7, kAllPhases, &sites, &bind);
    check(r.result == 0 && r.rt.function_hooks != nullptr, std::string("two mods share a before/after site: ") + r.rejection);
    if (r.result != 0) return;
    run_frames(r.rt, 1U);
    GenesisFunctionHooks *h = r.rt.function_hooks;
    check(h->enter(h->ctx, &r.rt, &sites[0], 0x00FFFF00U) == 0, "a shared site never replaces");
    h->leave(h->ctx, &r.rt, &sites[0], 0x00FFFF00U);
    const std::string first = std::to_string(a.digit), second = std::to_string(b.digit);
    check(log == std::vector<std::string>{first + "e", second + "e", first + "x", second + "x"}, "shared hooks run in composite order");
  }
  {
    const std::vector<std::vector<GenesisModHookBinding>> bind = {{{"swap", 1U}}, {{"swap", 1U}}};
    Rig r;
    r.attach({member(1), member(2)}, 7, kAllPhases, &sites, &bind);
    check(r.result == 1 && std::string(r.rejection) == "E_SET_HOOK_OWNER" && r.bad_member == 1U && r.rt.frame_hooks == nullptr && r.rt.function_hooks == nullptr, "a replace site has one exclusive owner");
  }
  {
    std::vector<std::string> log;
    Mod owner = member(2), watcher = member(1);
    owner.order = watcher.order = &log;
    owner.replace = 1;
    const std::vector<std::vector<GenesisModHookBinding>> bind = {{{"watch", 1U}}, {{"swap", 1U}}};
    Mod faulting = watcher;
    faulting.fault_phase = SEGA_MOD_PHASE_HOOK_ENTRY;
    Rig r;
    r.attach({faulting, owner}, 7, kAllPhases, &sites, &bind);
    check(r.result == 0, "different sites, different owners");
    if (r.result != 0) return;
    run_frames(r.rt, 1U);
    GenesisFunctionHooks *h = r.rt.function_hooks;
    check(h->enter(h->ctx, &r.rt, &sites[0], 0x00FFFF00U) == 0 && r.schedulers[0]->disabled, "a hook fault disables only its mod");
    check(h->enter(h->ctx, &r.rt, &sites[1], 0x00FFFF00U) == 1 && !r.schedulers[1]->disabled, "the other mod's replacement still commits");
    check(log == std::vector<std::string>{"1e", "2e"}, "only the bound mod is called at each site");
  }
}

std::vector<uint8_t> capture(Rig &r, GenesisSnapshotModState &seam, GenesisSnapshotStatus &status) {
  genesis_mod_set_snapshot_state(r.set.get(), &seam);
  GenesisSnapshotMachine machine{};
  machine.runtime = &r.rt;
  machine.mod = &seam;
  GenesisSnapshotIdentity id{};
  GenesisSnapshotBuffer buffer{nullptr, 0};
  status = genesis_snapshot_capture(&machine, &id, nullptr, &buffer);
  std::vector<uint8_t> out;
  if (status == GENESIS_SNAPSHOT_OK) out.assign(buffer.data, buffer.data + buffer.size);
  genesis_snapshot_buffer_free(&buffer);
  return out;
}
GenesisSnapshotStatus restore(Rig &r, const std::vector<uint8_t> &bytes) {
  GenesisSnapshotModState seam{};
  genesis_mod_set_snapshot_state(r.set.get(), &seam);
  GenesisSnapshotMachine machine{};
  machine.runtime = &r.rt;
  machine.mod = &seam;
  GenesisSnapshotIdentity id{};
  return genesis_snapshot_restore(&machine, &id, bytes.data(), static_cast<uint32_t>(bytes.size()));
}

void state() {
  const std::vector<Mod> spec = {member(1, 3), member(2, 5)};
  Rig a;
  a.attach(spec);
  run_frames(a.rt, 3U);
  GenesisSnapshotModState seam{};
  GenesisSnapshotStatus status{};
  const std::vector<uint8_t> saved = capture(a, seam, status);
  check(status == GENESIS_SNAPSHOT_OK && !saved.empty(), std::string("the set is capturable: ") + genesis_snapshot_status_text(status));
  run_frames(a.rt, 6U);
  const std::string end = guest_digest(a.rt), w0 = witness(*a.schedulers[0]), w1 = witness(*a.schedulers[1]);
  const uint32_t c0 = a.mods[0]->counter, c1 = a.mods[1]->counter;
  check(restore(a, saved) == GENESIS_SNAPSHOT_OK, "the same set restores its state");
  check(a.mods[0]->counter == 3U && a.mods[1]->counter == 3U, "every member's own blob comes back");
  run_frames(a.rt, 6U);
  check(guest_digest(a.rt) == end && witness(*a.schedulers[0]) == w0 && witness(*a.schedulers[1]) == w1 && a.mods[0]->counter == c0 && a.mods[1]->counter == c1, "replay after restore is identical");
  const struct { const char *name; std::vector<Mod> spec; uint8_t identity; } others[] = {
      {"another composite identity", spec, 8},
      {"another order (even under the same identity bytes)", {member(2, 5), member(1, 3)}, 7},
      {"another membership", {member(1, 3), member(3, 7)}, 7},
      {"a subset", {member(1, 3)}, 7},
  };
  for (const auto &o : others) {
    Rig r;
    r.attach(o.spec, o.identity);
    run_frames(r.rt, 2U);
    const std::string before = guest_digest(r.rt);
    const GenesisSnapshotStatus st = restore(r, saved);
    check(st == GENESIS_SNAPSHOT_MOD_MISMATCH && guest_digest(r.rt) == before, std::string("a state is refused by ") + o.name + ": " + genesis_snapshot_status_text(st));
  }
  Rig refusing;
  refusing.attach(spec);
  run_frames(refusing.rt, 5U);
  refusing.mods[1]->refuse_load = true;
  const std::string before = guest_digest(refusing.rt);
  check(restore(refusing, saved) == GENESIS_SNAPSHOT_MOD_REJECTED, "one member refusing refuses the restore");
  check(guest_digest(refusing.rt) == before && refusing.mods[0]->counter == 5U && refusing.mods[1]->counter == 5U && !refusing.schedulers[0]->disabled && !refusing.schedulers[1]->disabled,
        "all-or-nothing: the member already loaded is put back and the guest is untouched");
  // a faulted member's state keeps the others restorable
  Mod bad = member(2, 5);
  bad.fault_phase = SEGA_MOD_PHASE_FRAME_END;
  Rig faulted;
  faulted.attach({member(1, 3), bad});
  run_frames(faulted.rt, 3U);
  const std::vector<uint8_t> with_fault = capture(faulted, seam, status);
  check(status == GENESIS_SNAPSHOT_OK, "a set with a faulted member is capturable");
  run_frames(faulted.rt, 5U);
  const std::string faulted_end = guest_digest(faulted.rt);
  check(restore(faulted, with_fault) == GENESIS_SNAPSHOT_OK && faulted.schedulers[1]->disabled && !faulted.schedulers[0]->disabled, "the fault is part of the state");
  run_frames(faulted.rt, 5U);
  check(guest_digest(faulted.rt) == faulted_end, "replay with a faulted member is identical");
}

// Recorded, not asserted against a threshold (host speed is not a property of the product): the cost of the dispatch itself, measured with members whose callbacks do nothing.
void overhead() {
  constexpr uint64_t kFrames = 300;
  for (uint32_t n : {1U, 2U, 4U, 8U, 16U}) {
    std::vector<Mod> spec;
    for (uint32_t i = 0; i < n; ++i) {
      Mod m = member(i + 1U);
      m.quiet = true;
      spec.push_back(m);
    }
    Rig r;
    r.attach(spec);
    check(r.result == 0, "attach " + std::to_string(n) + " members");
    if (r.result != 0) continue;
    run_frames(r.rt, 1U);
    // the boundary alone (frame-end, render with its work-RAM copy skipped without frame.work_ram, and the next frame's opening phases): the guest instructions between boundaries are excluded
    const GenesisFrameHooks *h = r.rt.frame_hooks;
    const uint64_t calls_before = r.schedulers[0]->callbacks_run;
    const auto start = std::chrono::steady_clock::now();
    for (uint64_t f = 0; f < kFrames * 100U; ++f) {
      h->frame_end(h->ctx, &r.rt);
      h->after_snapshot(h->ctx, &r.rt);
    }
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / static_cast<double>(kFrames * 100U);
    check(r.schedulers[0]->callbacks_run == calls_before + kFrames * 100U * 5U, "five phase callbacks per member per frame boundary");
    std::printf("OVERHEAD mods=%u ns_per_frame_boundary=%.0f ns_per_callback=%.0f\n", n, ns, ns / (5.0 * n));
  }
}

}  // namespace

int main() {
  ordering_permutations();
  failure_isolation();
  attach_refusals();
  hooks();
  state();
  overhead();
  if (failures != 0) { std::printf("%d failures\n", failures); return 1; }
  std::printf("OK\n");
  return 0;
}
