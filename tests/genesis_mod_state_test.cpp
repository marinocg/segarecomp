// SEG-138-T006 (ADR 0299): mod-owned state in the Genesis save-state / rewind container (`MODSTATE`). A project-authored synthetic program and a native ABI 2 provider with a blob state
// (a frame counter) run under the ONE mod scheduler; the SEG-131 snapshot codec carries the scheduler state and the provider's blob. No ROM, no host clock.
#include "genesis_snapshot.h"
#include "genesis_viewer_state.h"
#include "mod_scheduler.h"
#include "runtime.h"
#include "sv_state.h"

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

constexpr uint64_t cap(unsigned id) { return UINT64_C(1) << id; }
constexpr uint32_t bit(uint32_t phase) { return 1U << phase; }

// ---- the synthetic program (the shape of genesis_mod_scheduler_test.cpp) ----
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
void run_to_frame(GenesisRuntime &r, uint64_t frame) {
  for (uint32_t guard = 0; r.virtual_frame_count < frame; ++guard) {
    if (guard > 100000U || genesis_runtime_run(&r, synthetic_dispatch, 997U).kind != GENESIS_RUNNER_RESOURCE_LIMIT) {
      check(false, "the synthetic program did not advance");
      return;
    }
  }
}

// ---- the native mod: a frame counter in its own memory, mirrored into work RAM; its state blob is the counter ----
struct ModMem {
  uint32_t counter = 0;
  bool refuse_load = false;     // refuses the NEXT load only (the restore blob); taking the previous state back then succeeds
  bool refuse_always = false;   // refuses every load: it cannot even take its own previous state back
  uint32_t loads = 0;
};
void mod_phase(void *state, const SegaModHost *h) {
  auto *m = static_cast<ModMem *>(state);
  if (h->phase == SEGA_MOD_PHASE_FRAME_BEGIN) {
    ++m->counter;
    h->write(SEGA_GEN_SPACE_M68K_WORK_RAM, 0x100, 4, m->counter);
  }
}
int mod_save(void *state, uint8_t *out, uint32_t cap_bytes, uint32_t *len) {
  const auto *m = static_cast<ModMem *>(state);
  if (cap_bytes < 4U) return 1;
  for (unsigned i = 0; i < 4U; ++i) out[i] = static_cast<uint8_t>(m->counter >> (8U * i));
  *len = 4U;
  return 0;
}
int mod_load(void *state, const uint8_t *in, uint32_t len) {
  auto *m = static_cast<ModMem *>(state);
  ++m->loads;
  if (m->refuse_always || len != 4U) return 1;
  if (m->refuse_load) { m->refuse_load = false; return 1; }
  m->counter = static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8U) | (static_cast<uint32_t>(in[2]) << 16U) | (static_cast<uint32_t>(in[3]) << 24U);
  return 0;
}
SegaModProvider make_provider(bool with_state) {
  SegaModProvider p;
  std::memset(&p, 0, sizeof p);
  p.size = sizeof p;
  p.abi_major = SEGA_MOD_ABI_MAJOR;
  p.platform = SEGA_MOD_PLATFORM_GENESIS;
  p.name = "state-mod";
  p.implementation_version = 1;
  p.phase = mod_phase;
  if (with_state) {
    p.save_state = mod_save;
    p.load_state = mod_load;
  }
  return p;
}
const GenesisModRange kRanges[] = {{SEGA_GEN_SPACE_M68K_WORK_RAM, 0x100U, 0x100U, GENESIS_MOD_ACCESS_READ | GENESIS_MOD_ACCESS_WRITE}};

const GenesisModRange kReadRanges[] = {{SEGA_GEN_SPACE_M68K_WORK_RAM, 0x100U, 0x100U, GENESIS_MOD_ACCESS_READ}};

struct Rig {
  std::unique_ptr<GenesisRuntime> rt = std::make_unique<GenesisRuntime>();   // 150 KB: heap, never the Windows 1 MB stack
  std::unique_ptr<GenesisModScheduler> sched = std::make_unique<GenesisModScheduler>();
  ModMem mem;
  SegaModProvider provider;
  GenesisSnapshotModState snap;
  GenesisSnapshotMachine machine;
  GenesisSnapshotIdentity id;
  std::string rejection;
  bool attached = false;
  Rig(uint8_t identity_byte, uint32_t schema, bool state_cap = true, bool write_cap = true) {
    setup(*rt);
    provider = make_provider(state_cap);
    GenesisModConfig c{};
    c.mod_id = "state-mod";
    c.provider = &provider;
    c.provider_state = &mem;
    c.phase_mask = bit(SEGA_MOD_PHASE_FRAME_BEGIN);
    c.required[0] = cap(SEGA_MOD_CAP_MEMORY_READ) | (write_cap ? cap(SEGA_MOD_CAP_MEMORY_WRITE) : 0U) | (state_cap ? cap(SEGA_MOD_CAP_STATE_MOD) : 0U);
    c.ranges = write_cap ? kRanges : kReadRanges;
    c.range_count = 1U;
    std::memset(c.identity_sha256, identity_byte, 32U);
    c.state_schema = state_cap ? schema : 0U;
    const char *why = "";
    attached = genesis_mod_scheduler_attach(sched.get(), rt.get(), &c, &why) == 0;
    rejection = why;
    genesis_mod_snapshot_state(sched.get(), &snap);
    std::memset(&machine, 0, sizeof machine);
    machine.runtime = rt.get();
    machine.mod = &snap;
    std::memset(&id, 0, sizeof id);
    std::memset(id.source_sha256, 0x5A, 32U);
  }
  ~Rig() { genesis_mod_scheduler_detach(sched.get()); }
};

struct Shot {
  GenesisSnapshotBuffer buf{nullptr, 0};
  ~Shot() { genesis_snapshot_buffer_free(&buf); }
  bool take(Rig &r) { return genesis_snapshot_capture(&r.machine, &r.id, nullptr, &buf) == GENESIS_SNAPSHOT_OK; }
  std::string digest() const {   // the container's own SHA-256 trailer: the "state hash"
    static const char *h = "0123456789abcdef";
    std::string s;
    for (uint32_t i = buf.size - 32U; i < buf.size; ++i) { s += h[buf.data[i] >> 4U]; s += h[buf.data[i] & 15U]; }
    return s;
  }
};

// Recomputes the SHA-256 trailer after a test edits bytes (a crafted but "intact" file: integrity is not authenticity).
void reseal(std::vector<uint8_t> &bytes) {
  GenesisSha256 sha;
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, bytes.data(), static_cast<uint32_t>(bytes.size() - 32U));
  genesis_sha256_final(&sha, bytes.data() + bytes.size() - 32U);
}
uint32_t le32(const uint8_t *b) { return b[0] | (static_cast<uint32_t>(b[1]) << 8U) | (static_cast<uint32_t>(b[2]) << 16U) | (static_cast<uint32_t>(b[3]) << 24U); }
// The MODSTATE section (id 11) of a container: offset and length.
bool find_modstate(const std::vector<uint8_t> &bytes, uint32_t &offset, uint32_t &length) {
  const uint32_t count = le32(bytes.data() + 28);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t *e = bytes.data() + 156U + i * 12U;
    if ((e[0] | (e[1] << 8U)) == 11) { offset = le32(e + 4); length = le32(e + 8); return true; }
  }
  return false;
}
std::string machine_hash(Rig &r) {   // everything restore may change: the container of the machine itself
  Shot s;
  return s.take(r) ? s.digest() : "capture-failed";
}

void round_trip() {
  Rig a(0x11, 7);
  check(a.attached, "the mod attaches with state.mod and a schema: " + a.rejection);
  run_to_frame(*a.rt, 6);
  Shot s1;
  check(s1.take(a), "a modded machine captures");
  GenesisSnapshotInfo info{};
  check(genesis_snapshot_inspect(s1.buf.data, s1.buf.size, &info) == GENESIS_SNAPSHOT_OK && (info.machine_flags & GENESIS_SNAPSHOT_FLAG_MOD) != 0U, "the container is flagged as carrying mod state");
  const uint32_t counter_at_save = a.mem.counter;
  check(counter_at_save >= 6U, "the mod advanced its own state (" + std::to_string(counter_at_save) + ")");
  run_to_frame(*a.rt, 14);
  Shot straight;
  check(straight.take(a), "capture at frame 14 (straight)");
  check(a.mem.counter > counter_at_save, "the mod state changed after the save");
  check(genesis_snapshot_restore(&a.machine, &a.id, s1.buf.data, s1.buf.size) == GENESIS_SNAPSHOT_OK, "restore of the saved state");
  check(a.mem.counter == counter_at_save, "the mod's own blob came back");
  Shot s1b;
  check(s1b.take(a) && s1b.digest() == s1.digest() && s1b.buf.size == s1.buf.size, "save, run, restore => the identical state hash including the mod state");
  run_to_frame(*a.rt, 14);
  Shot again;
  check(again.take(a) && again.digest() == straight.digest(), "restore then run reproduces the straight run's state hash at the same frame");
  uint8_t w1[32], w2[32];
  genesis_mod_write_log_digest(a.sched.get(), w1);
  Rig b(0x11, 7);
  run_to_frame(*b.rt, 14);
  genesis_mod_write_log_digest(b.sched.get(), w2);
  check(std::memcmp(w1, w2, 32U) == 0 && a.sched->write_total == b.sched->write_total, "the replay witness (write digest) continues exactly across a restore");

  // a state restores into a FRESH machine of the same mod (a new process): same hash afterwards
  Rig c(0x11, 7);
  check(genesis_snapshot_restore(&c.machine, &c.id, s1.buf.data, s1.buf.size) == GENESIS_SNAPSHOT_OK && c.mem.counter == counter_at_save, "restore into a fresh machine of the same mod");
  Shot s1c;
  check(s1c.take(c) && s1c.digest() == s1.digest(), "the fresh machine hashes identically after the restore");
}

void identity_and_schema() {
  Rig a(0x11, 7);
  run_to_frame(*a.rt, 5);
  Shot s;
  check(s.take(a), "capture");
  const std::string before_hash = machine_hash(a);
  struct Other { const char *what; uint8_t identity; uint32_t schema; GenesisSnapshotStatus want; };
  const Other others[] = {{"another mod identity", 0x22, 7, GENESIS_SNAPSHOT_MOD_MISMATCH}, {"another state schema", 0x11, 8, GENESIS_SNAPSHOT_MOD_MISMATCH}};
  for (const Other &o : others) {
    Rig b(o.identity, o.schema);
    run_to_frame(*b.rt, 3);
    const std::string hb = machine_hash(b);
    const uint32_t counter = b.mem.counter;
    const GenesisSnapshotStatus st = genesis_snapshot_restore(&b.machine, &b.id, s.buf.data, s.buf.size);
    check(st == o.want, std::string("restore with ") + o.what + " is refused with E_STATE_MOD_MISMATCH (got " + std::to_string(st) + ")");
    check(std::strstr(genesis_snapshot_status_text(st), "E_STATE_MOD_MISMATCH") != nullptr, "the diagnostic is stable text");
    check(machine_hash(b) == hb && b.mem.counter == counter && b.mem.loads == 0U, std::string("the machine is unchanged and the mod's load was never called (") + o.what + ")");
    check(genesis_snapshot_validate(&b.machine, &b.id, s.buf.data, s.buf.size, nullptr) == o.want, std::string("validate agrees (") + o.what + ")");
  }
  // a modless build refuses a modded state, and a modded build refuses a vanilla state
  {
    Rig v(0x11, 7);
    GenesisSnapshotMachine plain{};
    plain.runtime = v.rt.get();
    genesis_mod_scheduler_detach(v.sched.get());
    check(genesis_snapshot_restore(&plain, &v.id, s.buf.data, s.buf.size) == GENESIS_SNAPSHOT_MOD_MISMATCH, "a build without the mod refuses a modded state");
    Shot vanilla;
    check(genesis_snapshot_capture(&plain, &v.id, nullptr, &vanilla.buf) == GENESIS_SNAPSHOT_OK, "a build without mods captures a vanilla container");
    GenesisSnapshotInfo vi{};
    check(genesis_snapshot_inspect(vanilla.buf.data, vanilla.buf.size, &vi) == GENESIS_SNAPSHOT_OK && (vi.machine_flags & GENESIS_SNAPSHOT_FLAG_MOD) == 0U, "the vanilla container carries no MODSTATE");
    check(genesis_snapshot_restore(&a.machine, &a.id, vanilla.buf.data, vanilla.buf.size) == GENESIS_SNAPSHOT_MOD_MISMATCH, "a modded build refuses a vanilla state");
  }
  check(machine_hash(a) == before_hash, "the modded machine is unchanged by every refusal");
  // other execution compatibility: different declared ranges are another build
  {
    Rig d(0x11, 7);
    d.snap.compat[0] ^= 1U;   // a different capability/phase/range declaration (the compat digest is part of the fingerprint)
    check(genesis_snapshot_restore(&d.machine, &d.id, s.buf.data, s.buf.size) != GENESIS_SNAPSHOT_OK, "a different compatibility digest is refused");
  }
}

void unavailable_and_disabled() {
  Rig nostate(0x11, 0, false, true);   // memory.write without state.mod: guest-affecting without state
  check(nostate.attached, "a guest-affecting mod without state.mod still attaches (it just cannot be saved)");
  run_to_frame(*nostate.rt, 2);
  Shot s;
  GenesisSnapshotBuffer b{nullptr, 0};
  const GenesisSnapshotStatus st = genesis_snapshot_capture(&nostate.machine, &nostate.id, nullptr, &b);
  check(st == GENESIS_SNAPSHOT_MOD_UNAVAILABLE && b.data == nullptr, "capture refuses with E_STATE_UNAVAILABLE");
  check(std::strstr(genesis_snapshot_status_text(st), "E_STATE_UNAVAILABLE") != nullptr, "stable text");

  // a read-only mod needs no state.mod and saves its flags
  Rig ro(0x33, 0, false, false);
  check(ro.attached, "read-only mod attaches");
  run_to_frame(*ro.rt, 4);
  Shot sro;
  check(sro.take(ro), "a read-only mod without state.mod captures (its disabled flag is its state)");

  // a faulted (disabled) mod round-trips as disabled, with its diagnostic
  Rig f(0x11, 7);
  run_to_frame(*f.rt, 3);
  f.provider.phase = [](void *, const SegaModHost *h) { uint32_t v = 0; h->read(SEGA_GEN_SPACE_M68K_WORK_RAM, 0x800, 1, &v); };   // outside every declared range: a fault
  run_to_frame(*f.rt, 6);
  check(f.sched->disabled == 1U && f.sched->fault.faulted == 1U, "the induced violation disabled the mod");
  Shot fs;
  check(fs.take(f), "a disabled mod captures");
  Rig g(0x11, 7);
  run_to_frame(*g.rt, 2);
  check(genesis_snapshot_restore(&g.machine, &g.id, fs.buf.data, fs.buf.size) == GENESIS_SNAPSHOT_OK, "restore of a disabled-mod state");
  check(g.sched->disabled == 1U && g.sched->fault.faulted == 1U && std::strcmp(g.sched->fault.text, f.sched->fault.text) == 0 && std::strcmp(g.sched->fault.cause, "range") == 0,
        "disabled flag and stable E_MOD_FAULT diagnostic are restored");
  const uint32_t counter = g.mem.counter;
  run_to_frame(*g.rt, 9);
  check(g.mem.counter == counter, "a restored-disabled mod is inert");
  Shot gs;
  check(gs.take(g), "recapture");
}

// Edit the MODSTATE body of a valid container, reseal, and require the stable refusal with the machine untouched.
void malformed_and_rejected() {
  Rig a(0x11, 7);
  run_to_frame(*a.rt, 5);
  Shot s;
  check(s.take(a), "capture");
  std::vector<uint8_t> good(s.buf.data, s.buf.data + s.buf.size);
  uint32_t off = 0, len = 0;
  check(find_modstate(good, off, len) && len > 44U, "the container has a MODSTATE section");
  const std::string hash = machine_hash(a);
  const uint32_t counter = a.mem.counter;
  struct Edit { const char *what; uint32_t at; uint8_t value; };
  const Edit edits[] = {
      {"disabled flag without a fault", 44U + 0U, 1U},   {"started flag out of range", 44U + 1U, 7U},         {"pad latch without an input mod", 44U + 3U, 1U},
      {"hook skip bit out of range", 44U + 12U + 8U + 16U + 32U, 0xF0U},
  };
  for (const Edit &e : edits) {
    std::vector<uint8_t> bad = good;
    bad[off + e.at] = e.value;
    reseal(bad);
    const GenesisSnapshotStatus st = genesis_snapshot_restore(&a.machine, &a.id, bad.data(), bad.size());
    check(st == GENESIS_SNAPSHOT_MOD_REJECTED, std::string("crafted state (") + e.what + ") is refused with E_STATE_MOD_REJECTED, got " + std::to_string(st));
    check(machine_hash(a) == hash && a.mem.counter == counter, std::string("machine unchanged after (") + e.what + ")");
  }
  {   // a body whose declared blob length lies
    std::vector<uint8_t> bad = good;
    bad[off + len - 8U] = 0x03U;   // blob_len u32 (4 bytes) sits right before the 4 blob bytes: make it 3
    reseal(bad);
    check(genesis_snapshot_restore(&a.machine, &a.id, bad.data(), bad.size()) == GENESIS_SNAPSHOT_MOD_REJECTED && machine_hash(a) == hash, "a blob length that disagrees with the section is refused");
  }
  {   // truncated section body (declared length shrinks by moving the trailer): the container itself rejects it
    std::vector<uint8_t> bad = good;
    bad.erase(bad.begin() + off + len - 1U);
    reseal(bad);
    check(genesis_snapshot_restore(&a.machine, &a.id, bad.data(), bad.size()) != GENESIS_SNAPSHOT_OK && machine_hash(a) == hash, "a truncated container is refused");
  }
  {   // a flipped payload byte without resealing: integrity
    std::vector<uint8_t> bad = good;
    bad[off + len - 2U] ^= 1U;
    check(genesis_snapshot_restore(&a.machine, &a.id, bad.data(), bad.size()) == GENESIS_SNAPSHOT_INTEGRITY, "a corrupted MODSTATE byte fails the integrity digest");
  }
  {   // the mod refuses its own state: nothing of the guest changes, the previous mod state is put back
    a.mem.refuse_load = true;
    run_to_frame(*a.rt, 9);
    const std::string h2 = machine_hash(a);
    const uint32_t counter2 = a.mem.counter;
    check(genesis_snapshot_restore(&a.machine, &a.id, s.buf.data, s.buf.size) == GENESIS_SNAPSHOT_MOD_REJECTED, "a mod that refuses its blob makes the whole restore E_STATE_MOD_REJECTED");
    check(machine_hash(a) == h2 && a.mem.counter == counter2 && a.sched->disabled == 0U, "guest and mod state are unchanged after the mod's refusal (the previous state was put back)");
    // a mod that cannot take even its previous state back is disabled (cause `state`); the guest is still untouched
    a.mem.refuse_always = true;
    const uint64_t frame = a.rt->virtual_frame_count;
    uint8_t ram_before[65536];
    std::memcpy(ram_before, a.rt->work_ram, sizeof ram_before);
    check(genesis_snapshot_restore(&a.machine, &a.id, s.buf.data, s.buf.size) == GENESIS_SNAPSHOT_MOD_REJECTED, "the refusal still rejects the restore");
    check(a.sched->disabled == 1U && std::strcmp(a.sched->fault.cause, "state") == 0 && a.rt->virtual_frame_count == frame && std::memcmp(ram_before, a.rt->work_ram, sizeof ram_before) == 0,
          "the untrustworthy mod is disabled (E_MOD_FAULT cause=state) and no guest memory or clock changed");
    a.mem.refuse_always = false;
  }
}

void hook_frames_round_trip() {
  static const GenesisHookSiteInfo sites[2] = {
      {0x400U, "a", GENESIS_HOOK_MODE_BEFORE, 0U, 0U, 0U, 0U, 0U},
      {0x500U, "b", GENESIS_HOOK_MODE_AFTER, 0U, 0U, 0U, 0U, 0U},
  };
  static const GenesisModHookBinding bindings[1] = {{"b", 1U}};
  auto rt = std::make_unique<GenesisRuntime>();
  auto sched = std::make_unique<GenesisModScheduler>();
  setup(*rt);
  ModMem mem;
  SegaModProvider p = make_provider(true);
  p.hook = [](void *, const SegaModHost *, uint32_t) { return 0; };
  GenesisModConfig c{};
  c.mod_id = "hook-state";
  c.provider = &p;
  c.provider_state = &mem;
  c.phase_mask = bit(SEGA_MOD_PHASE_HOOK_EXIT);
  c.required[0] = cap(SEGA_MOD_CAP_HOOK_FUNCTION) | cap(SEGA_MOD_CAP_STATE_MOD);
  c.state_schema = 1U;
  c.hooks = bindings;
  c.hook_count = 1U;
  c.sites = sites;
  c.site_count = 2U;
  const char *why = "";
  check(genesis_mod_scheduler_attach(sched.get(), rt.get(), &c, &why) == 0, std::string("hook mod attaches: ") + why);
  GenesisSnapshotModState snap;
  genesis_mod_snapshot_state(sched.get(), &snap);
  GenesisSnapshotMachine m{};
  m.runtime = rt.get();
  m.mod = &snap;
  GenesisSnapshotIdentity id{};
  sched->function_hooks.depth = 2U;
  sched->function_hooks.frames[0] = {&sites[1], 0x1234U, 0xFFFE00U};
  sched->function_hooks.frames[1] = {&sites[1], 0x2345U, 0xFFFD00U};
  sched->function_hooks.abandoned = 3U;
  GenesisSnapshotBuffer buf{nullptr, 0};
  check(genesis_snapshot_capture(&m, &id, nullptr, &buf) == GENESIS_SNAPSHOT_OK, "a state with two hook frames in flight captures");
  sched->function_hooks.depth = 0U;
  sched->function_hooks.abandoned = 0U;
  check(genesis_snapshot_restore(&m, &id, buf.data, buf.size) == GENESIS_SNAPSHOT_OK, "and restores");
  check(sched->function_hooks.depth == 2U && sched->function_hooks.frames[1].site == &sites[1] && sched->function_hooks.frames[1].return_pc == 0x2345U &&
            sched->function_hooks.frames[0].entry_sp == 0xFFFE00U && sched->function_hooks.abandoned == 3U,
        "the pending exits (site by index, return pc, entry SP) and counters are restored");
  genesis_snapshot_buffer_free(&buf);
  // a site table that is not available cannot carry a frame: fail closed
  sched->config.site_count = 0U;
  sched->function_hooks.depth = 1U;
  check(genesis_snapshot_capture(&m, &id, nullptr, &buf) == GENESIS_SNAPSHOT_NOT_CAPTURABLE, "a frame in flight without a site table is not capturable");
  sched->function_hooks.depth = 0U;
  genesis_mod_scheduler_detach(sched.get());
}

// ---- rewind through the viewer state service (the SEG-131 engine) ----
struct RewindHost {
  int restored = 0;
};
void after_restore(void *ctx) { ++static_cast<RewindHost *>(ctx)->restored; }

void rewind_through_a_mod_state_change() {
  Rig a(0x11, 7);
  RewindHost host;
  GenesisViewerStateConfig cfg{};
  cfg.machine = a.machine;
  cfg.identity = a.id;
  cfg.state_dir = nullptr;
  cfg.unavailable_reason = nullptr;
  cfg.after_restore = after_restore;
  cfg.after_restore_context = &host;
  cfg.rewind_budget_bytes = 8U * 1024U * 1024U;
  cfg.rewind_interval_ms = 100U;   // guest time
  cfg.rewind_target_ms = 60000U;
  GenesisViewerState *vs = genesis_viewer_state_create(&cfg);
  check(vs != nullptr, "the viewer state service is created for a modded machine");
  if (vs == nullptr) return;
  SvStateProvider prov{};
  genesis_viewer_state_provider(vs, &prov);
  // 12 frames at ~16.7 ms: a moment about every 6 frames; the mod counter differs in each
  std::vector<uint32_t> counters_at_capture;
  std::vector<std::string> hashes;
  uint64_t last_captures = 0;
  for (uint64_t frame = 1; frame <= 30; ++frame) {
    run_to_frame(*a.rt, frame);
    genesis_viewer_state_after_slice(vs);
    GenesisViewerStateStats st{};
    genesis_viewer_state_stats(vs, &st);
    if (st.captures != last_captures) {
      last_captures = st.captures;
      counters_at_capture.push_back(a.mem.counter);
      hashes.push_back(machine_hash(a));
    }
  }
  const int count = prov.history_count(prov.ctx);
  check(count >= 3 && static_cast<size_t>(count) == counters_at_capture.size(), "rewind recorded several moments with different mod state (" + std::to_string(count) + ")");
  if (count < 3) { genesis_viewer_state_destroy(vs); return; }
  check(counters_at_capture.front() != counters_at_capture.back(), "the mod state differs between the first and the last moment");
  const std::string live_before = machine_hash(a);
  const int pick = 1;   // a middle moment: both older and newer moments exist
  char err[96] = "";
  check(prov.restore_entry(prov.ctx, pick, err, sizeof err) == 0, std::string("rewind restore: ") + err);
  check(a.mem.counter == counters_at_capture[static_cast<size_t>(pick)], "rewind restored the mod's own state exactly (" + std::to_string(a.mem.counter) + " == " + std::to_string(counters_at_capture[static_cast<size_t>(pick)]) + ")");
  check(machine_hash(a) == hashes[static_cast<size_t>(pick)], "the whole machine (guest + mod) hashes exactly as it did at that moment");
  check(host.restored == 1 && machine_hash(a) != live_before, "the after-restore hook ran and the present moved");
  check(prov.history_count(prov.ctx) == pick + 1, "the resumed future is a new history branch");
  // the resumed future from the rewound moment is the straight future: run on and compare with an unrewound twin at the same frame
  const uint64_t resume_frame = a.rt->virtual_frame_count;
  Rig twin(0x11, 7);
  run_to_frame(*twin.rt, resume_frame + 4U);
  run_to_frame(*a.rt, resume_frame + 4U);
  check(a.mem.counter == twin.mem.counter, "after rewinding, the mod continues exactly as the straight run does");
  genesis_viewer_state_destroy(vs);
}

void limits_and_negative_attach() {
  {
    Rig bad(0x11, 0, true, true);   // state.mod granted but schema 0
    check(!bad.attached && bad.rejection == "E_STATE_SCHEMA", "state.mod without a schema is refused at attach (E_STATE_SCHEMA): " + bad.rejection);
  }
  {
    auto rt = std::make_unique<GenesisRuntime>();
    auto sched = std::make_unique<GenesisModScheduler>();
    setup(*rt);
    SegaModProvider p = make_provider(false);   // no save/load entries
    ModMem mem;
    GenesisModConfig c{};
    c.mod_id = "x";
    c.provider = &p;
    c.provider_state = &mem;
    c.phase_mask = bit(SEGA_MOD_PHASE_FRAME_BEGIN);
    c.required[0] = cap(SEGA_MOD_CAP_STATE_MOD);
    c.state_schema = 1U;
    const char *why = "";
    check(genesis_mod_scheduler_attach(sched.get(), rt.get(), &c, &why) != 0 && std::string(why) == "E_PROVIDER_ENTRY", "state.mod without save_state/load_state is refused (E_PROVIDER_ENTRY)");
  }
  {   // an oversize provider blob is refused at capture, never truncated
    Rig a(0x11, 7);
    a.provider.save_state = [](void *, uint8_t *, uint32_t, uint32_t *len) { *len = 70000U; return 0; };
    run_to_frame(*a.rt, 2);
    GenesisSnapshotBuffer b{nullptr, 0};
    check(genesis_snapshot_capture(&a.machine, &a.id, nullptr, &b) == GENESIS_SNAPSHOT_MOD_UNAVAILABLE, "a blob over 64 KiB is refused (E_STATE_UNAVAILABLE), not truncated");
  }
}

}  // namespace

int main() {
  round_trip();
  identity_and_schema();
  unavailable_and_disabled();
  malformed_and_rejected();
  hook_frames_round_trip();
  rewind_through_a_mod_state_change();
  limits_and_negative_attach();
  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("genesis_mod_state_tests: ok\n");
  return 0;
}
