// SEG-147-T004/T005/T006 (ADR 0325): the Master System mod scheduler and its save-state/rewind seam, driven directly over the real machine, devices, snapshot codec and compositor with a scripted CPU
// (mod_platform_conformance_sms_cpu.c). What the built-program test (mod_sms_lifecycle_test.py) cannot reach: pad interception at the port, cartridge RAM by mapper, machine reset, every state refusal,
// a mod whose own load fails, a disabled mod's state, damaged MODSTATE bodies and a rewind across a mod-state change. Synthetic data only.
#include "sms_mod_scheduler.h"
#include "sms_psg.h"
#include "sms_render.h"
#include "sms_snapshot.h"
#include "segarecomp/sha256.hpp"

#include <algorithm>
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
constexpr std::uint64_t cap(unsigned id) { return std::uint64_t{1} << id; }
constexpr std::uint32_t bit(std::uint32_t phase) { return 1U << phase; }
constexpr std::uint32_t kAllPhases = bit(1) | bit(2) | bit(3) | bit(4) | bit(7) | bit(8);

struct Mod {
  std::uint32_t counter = 0;      // the mod's own state; it decides what the mod writes
  std::vector<std::string> trace;
  const SegaModHost *host = nullptr;
  bool fail_load = false;
  int cart_status = -1;
  std::uint32_t cart_value = 0, post_input = 0, previous = 99;
  bool intercept = false;
  void (*extra)(Mod &, const SegaModHost *) = nullptr;
};

void phase(void *state, const SegaModHost *h) {
  Mod &m = *static_cast<Mod *>(state);
  m.trace.push_back(std::to_string(h->phase) + ":" + std::to_string(h->frame));
  if (h->phase == SEGA_MOD_PHASE_FRAME_BEGIN && sega_mod_is_granted(h, SEGA_MOD_CAP_INPUT_READ)) h->input_get(1, &m.previous);
  if (h->phase == SEGA_MOD_PHASE_INPUT && m.intercept) h->input_set(1, SEGA_SMS_BUTTON_UP);
  if (h->phase == SEGA_MOD_PHASE_POST_INPUT && sega_mod_is_granted(h, SEGA_MOD_CAP_INPUT_READ)) h->input_get(1, &m.post_input);
  if (h->phase == SEGA_MOD_PHASE_FRAME_END && sega_mod_is_granted(h, SEGA_MOD_CAP_MEMORY_WRITE)) {
    ++m.counter;
    h->write(SEGA_SMS_SPACE_Z80_WORK_RAM, 0x10, 2, m.counter * 3U);
  }
  if (m.extra != nullptr) m.extra(m, h);
}
int save(void *state, std::uint8_t *out, std::uint32_t capacity, std::uint32_t *len) {
  const Mod &m = *static_cast<Mod *>(state);
  if (capacity < 4) return 1;
  for (unsigned i = 0; i < 4; ++i) out[i] = static_cast<std::uint8_t>(m.counter >> (8U * i));
  *len = 4;
  return 0;
}
int load(void *state, const std::uint8_t *in, std::uint32_t len) {
  Mod &m = *static_cast<Mod *>(state);
  if (len != 4 || m.fail_load) { m.fail_load = false; return 1; }   // refuses once: the scheduler puts the previous blob back through the same entry
  m.counter = static_cast<std::uint32_t>(in[0]) | (static_cast<std::uint32_t>(in[1]) << 8U) | (static_cast<std::uint32_t>(in[2]) << 16U) | (static_cast<std::uint32_t>(in[3]) << 24U);
  return 0;
}

constexpr std::uint64_t kReadWrite = cap(SEGA_MOD_CAP_MEMORY_READ) | cap(SEGA_MOD_CAP_MEMORY_WRITE);   // the default range is read-write
constexpr std::uint64_t kStateful = kReadWrite | cap(SEGA_MOD_CAP_STATE_MOD);

struct Rig {
  std::vector<std::uint8_t> rom = std::vector<std::uint8_t>(0x8000, 0);
  std::unique_ptr<SmsMachine> machine = std::make_unique<SmsMachine>();
  std::unique_ptr<SmsVdp> vdp = std::make_unique<SmsVdp>();
  std::unique_ptr<SmsRenderer> renderer = std::make_unique<SmsRenderer>();
  std::unique_ptr<SmsPsg> psg = std::make_unique<SmsPsg>();
  std::unique_ptr<SmsModScheduler> sched = std::make_unique<SmsModScheduler>();
  SmsPad pad{};
  Mod mod;
  SegaModProvider provider{};
  SmsModState state{};
  SmsSnapshotMachine snapshot{};
  SmsSnapshotIdentity id{};
  std::vector<SmsModRange> ranges{{SEGA_SMS_SPACE_Z80_WORK_RAM, 0x10, 4, SMS_MOD_ACCESS_READ | SMS_MOD_ACCESS_WRITE}};
  std::uint32_t version = 1;
  std::uint8_t identity_byte = 0x11;

  explicit Rig(SmsMapperFamily family = SMS_MAPPER_ROM_ONLY) {
    std::memset(vdp.get(), 0, sizeof(SmsVdp));
    sms_machine_init(machine.get(), rom.data(), static_cast<std::uint32_t>(rom.size()), family);
    sms_vdp_install(machine.get(), vdp.get());
    sms_renderer_init(renderer.get(), vdp.get(), nullptr, 0);
    sms_renderer_attach(renderer.get());
    sms_psg_attach(psg.get(), machine.get(), nullptr, nullptr);
    sms_pad_attach(machine.get(), &pad);
    id.source_sha256[0] = 0x77;
    snapshot = {machine.get(), vdp.get(), renderer.get(), psg.get(), &pad, 0, nullptr};
  }
  void start() {
    sms_renderer_init(renderer.get(), vdp.get(), nullptr, 0);   // as the viewer's reset does (sms_viewer_rig_reset)
    sms_renderer_attach(renderer.get());
    sms_machine_reset(machine.get());
    vdp->reg[0] = 0x06;   // Mode 4: the scripted CPU programs nothing
    vdp->reg[1] = 0x80;
  }
  std::string attach(std::uint64_t caps, std::uint32_t phases = kAllPhases) {
    provider = {};
    provider.size = sizeof provider;
    provider.abi_major = SEGA_MOD_ABI_MAJOR;
    provider.platform = SEGA_MOD_PLATFORM_MASTER_SYSTEM;
    provider.name = "test";
    provider.implementation_version = version;
    provider.phase = phase;
    const bool stateful = (caps & cap(SEGA_MOD_CAP_STATE_MOD)) != 0;
    if (stateful) { provider.save_state = save; provider.load_state = load; }
    SmsModConfig c{};
    c.mod_id = "test";
    c.identity_sha256[0] = identity_byte;
    c.provider = &provider;
    c.provider_state = &mod;
    c.phase_mask = phases;
    c.required[0] = caps;
    c.ranges = ranges.data();
    c.range_count = (caps & (cap(SEGA_MOD_CAP_MEMORY_READ) | cap(SEGA_MOD_CAP_MEMORY_WRITE))) != 0 ? static_cast<std::uint32_t>(ranges.size()) : 0U;
    c.state_schema = stateful ? 1U : 0U;
    const char *why = "";
    if (sms_mod_scheduler_attach(sched.get(), machine.get(), &pad, vdp.get(), &c, &why) != 0) return why;
    sms_mod_scheduler_state(sched.get(), &state);
    snapshot.mod = &state;
    start();
    return "";
  }
  void run(std::uint64_t frame) {
    const SmsStop stop = sms_run_until_frame(machine.get(), frame);
    sms_psg_sync(psg.get(), stop.cycles);
  }
  std::uint32_t word() const { return static_cast<std::uint32_t>(machine->mem.ram[0x10]) | (static_cast<std::uint32_t>(machine->mem.ram[0x11]) << 8U); }
  std::vector<std::uint8_t> capture(SmsSnapshotStatus *status = nullptr) {
    SmsSnapshotBuffer b{nullptr, 0};
    const SmsSnapshotStatus st = sms_snapshot_capture(&snapshot, &id, nullptr, &b);
    if (status != nullptr) *status = st;
    std::vector<std::uint8_t> out(b.data, b.data + b.size);
    sms_snapshot_buffer_free(&b);
    return out;
  }
  SmsSnapshotStatus restore(const std::vector<std::uint8_t> &bytes) { return sms_snapshot_restore(&snapshot, &id, bytes.data(), bytes.size()); }
  std::string digest() {
    std::uint8_t d[32];
    sms_machine_digest(machine.get(), d);
    return std::string(reinterpret_cast<const char *>(d), 32);
  }
  ~Rig() { sms_mod_scheduler_detach(sched.get()); }
};

// Rewrites the container's trailing digest after a deliberate edit, so the MOD codec (not the file integrity check) is what refuses it.
void reseal(std::vector<std::uint8_t> &bytes) {
  SmsSha256 sha;
  sms_sha256_init(&sha);
  sms_sha256_update(&sha, bytes.data(), bytes.size() - 32);
  sms_sha256_final(&sha, bytes.data() + bytes.size() - 32);
}

void test_phases_input_and_reset() {
  Rig r;
  r.mod.intercept = true;
  check(r.attach(cap(SEGA_MOD_CAP_INPUT_READ) | cap(SEGA_MOD_CAP_INPUT_INTERCEPT)).empty(), "attach");
  static const SmsInputEvent events[] = {{1, SMS_PAD_BUTTON1, 0, 0}};
  sms_machine_set_input(r.machine.get(), events, 1);
  r.run(2);
  const std::vector<std::string> expect = {"1:0", "2:0", "3:0", "4:0", "7:0", "8:0", "2:1", "3:1", "4:1", "7:1", "8:1", "2:2", "3:2", "4:2"};
  check(r.mod.trace == expect, "phase order: reset once, then frame-begin, input, post-input; frame-end and render close the frame before the next begins");
  check(r.mod.previous == SMS_PAD_UP && r.mod.post_input == SMS_PAD_UP, "frame-begin sees the previous FINAL input, post-input the final one");
  check(r.machine->input.p1 == SMS_PAD_BUTTON1, "the physical input (what is recorded, digested and saved) is the unmodified one");
  check((sms_pad_read(&r.pad, SMS_PORT_PAD_DC, 0) & 0x3F) == 0x3E, "the guest's port reads the mod's input: only Up pressed (active low)");
  r.mod.trace.clear();
  sms_machine_reset(r.machine.get());
  check(r.pad.intercept == nullptr, "a machine reset drops the interception");
  r.vdp->reg[0] = 0x06;
  r.vdp->reg[1] = 0x80;
  r.run(0);
  check(r.mod.trace == std::vector<std::string>({"1:0", "2:0", "3:0", "4:0"}), "a machine reset runs `reset` again before frame 0");
  Rig plain;
  check(plain.attach(cap(SEGA_MOD_CAP_HOST_LOG), bit(SEGA_MOD_PHASE_FRAME_END)).empty() && (plain.run(3), plain.pad.intercept == nullptr), "a mod without an `input` phase never touches the pad");
}

void faulting_cart(Mod &m, const SegaModHost *h) {
  if (h->phase != SEGA_MOD_PHASE_FRAME_BEGIN || h->frame != 1) return;
  m.cart_status = h->read(SEGA_SMS_SPACE_Z80_CART_RAM, 5, 2, &m.cart_value);
}

void test_cart_ram_and_negotiation() {
  {
    Rig r(SMS_MAPPER_SEGA);
    r.ranges = {{SEGA_SMS_SPACE_Z80_CART_RAM, 4, 4, SMS_MOD_ACCESS_READ}};
    r.mod.extra = faulting_cart;
    check(r.attach(cap(SEGA_MOD_CAP_MEMORY_READ), bit(SEGA_MOD_PHASE_FRAME_BEGIN)).empty(), "a Sega-mapper program declares a cartridge RAM range");
    r.machine->mem.cart_ram[5] = 0x34;
    r.machine->mem.cart_ram[6] = 0x12;
    r.run(2);
    check(r.mod.cart_status == SEGA_MOD_OK && r.mod.cart_value == 0x1234 && r.sched->fault.faulted == 0U, "cartridge RAM reads little-endian inside the declared range");
    const auto *f = static_cast<const SegaMasterSystemFeatures *>(r.sched->host.platform_features);
    check(f->cart_ram_bytes == 32768 && f->mapper == SEGA_SMS_MAPPER_SEGA && f->render == nullptr && f->vdp == nullptr, "the feature table reports the mapper; no presentation table without its capability");
  }
  {
    Rig r;
    r.ranges = {{SEGA_SMS_SPACE_Z80_CART_RAM, 0, 4, SMS_MOD_ACCESS_READ}};
    check(r.attach(cap(SEGA_MOD_CAP_MEMORY_READ)) == "E_RANGE_DECLARATION", "rom_only has no cartridge RAM");
    r.ranges = {{SEGA_SMS_SPACE_Z80_WORK_RAM, 8190, 3, SMS_MOD_ACCESS_READ}};
    check(r.attach(cap(SEGA_MOD_CAP_MEMORY_READ)) == "E_RANGE_DECLARATION", "a range past the 8 KiB is refused");
    check(r.attach(cap(SEGA_MOD_CAP_HOOK_FUNCTION)) == "E_CAPABILITY_UNSUPPORTED", "function hooks are not supported");
    check(r.attach(cap(SEGA_MOD_CAP_HOST_LOG), bit(SEGA_MOD_PHASE_HOOK_ENTRY)) == "E_PHASE_UNSUPPORTED", "no hook phases");
    check(r.machine->frame_hooks == nullptr && r.pad.intercept == nullptr, "a refused attach leaves the machine untouched");
  }
}

void test_state_and_rewind() {
  Rig r;
  check(r.attach(kStateful).empty(), "attach");
  r.run(5);
  const std::vector<std::uint8_t> at5 = r.capture();
  const std::string digest5 = r.digest();
  check(r.mod.counter == 5 && r.word() == 15 && !at5.empty(), "five frames: the mod's counter and the word it wrote agree");
  r.run(9);
  const std::vector<std::uint8_t> at9 = r.capture();
  std::uint8_t witness9[32];
  sms_mod_write_log_digest(r.sched.get(), witness9);
  r.run(20);
  // rewind across a mod state change: back to 5, forward to 9 again
  check(r.restore(at5) == SMS_SNAPSHOT_OK && r.mod.counter == 5 && r.word() == 15 && r.digest() == digest5, "rewind restores the guest AND the mod's state together");
  check(r.capture() == at5, "the restored canonical state is byte-identical");
  r.run(9);
  std::uint8_t witness_again[32];
  sms_mod_write_log_digest(r.sched.get(), witness_again);
  check(r.capture() == at9 && std::memcmp(witness9, witness_again, 32) == 0, "the re-run future equals the original one, mod state and replay witness included");

  // refusals: nothing changes
  const auto unchanged = [&](const char *what, const std::vector<std::uint8_t> &bytes, SmsSnapshotStatus want) {
    const std::vector<std::uint8_t> before = r.capture();
    const std::uint32_t counter = r.mod.counter;
    const SmsSnapshotStatus st = r.restore(bytes);
    check(st == want, std::string(what) + ": " + sms_snapshot_status_text(st));
    check(r.capture() == before && r.mod.counter == counter && r.sched->disabled == 0, std::string(what) + ": machine and mod unchanged");
  };
  {
    Rig other;
    other.identity_byte = 0x22;
    check(other.attach(kStateful).empty(), "another mod");
    other.run(5);
    unchanged("another mod identity", other.capture(), SMS_SNAPSHOT_MOD_MISMATCH);
    Rig newer;
    newer.version = 2;
    check(newer.attach(kStateful).empty(), "another provider version");
    newer.run(5);
    unchanged("the same identity with another compatibility digest", newer.capture(), SMS_SNAPSHOT_MOD_MISMATCH);
    Rig vanilla;
    vanilla.start();
    vanilla.run(5);
    const std::vector<std::uint8_t> plain = vanilla.capture();
    SmsSnapshotInfo info;
    check(sms_snapshot_inspect(plain.data(), plain.size(), &info) == SMS_SNAPSHOT_OK && info.has_mod == 0 && plain[28] == 7, "a state without a mod has no MODSTATE section (the container is unchanged)");
    unchanged("a Vanilla state in a modded program", plain, SMS_SNAPSHOT_MOD_MISMATCH);
    check(vanilla.restore(at5) == SMS_SNAPSHOT_MOD_MISMATCH, "a modded state in a Vanilla program is refused");
    check(sms_snapshot_inspect(at5.data(), at5.size(), &info) == SMS_SNAPSHOT_OK && info.has_mod == 1 && info.mod_state_schema == 1 && info.mod_identity[0] == 0x11, "inspect reports the mod");
  }
  r.mod.fail_load = true;
  unchanged("a mod that refuses its own state", at5, SMS_SNAPSHOT_MOD_REJECTED);
  {
    // the MODSTATE body is the last section: its flags start 68 bytes (identity, schema, compat) into it. Find it through the section table.
    const std::size_t entry = 156 + 7 * 12;
    const std::size_t offset = static_cast<std::size_t>(at5[entry + 4]) | (static_cast<std::size_t>(at5[entry + 5]) << 8U) | (static_cast<std::size_t>(at5[entry + 6]) << 16U);
    std::vector<std::uint8_t> bad = at5;
    bad[offset + 68] = 2;   // `disabled` is a boolean
    reseal(bad);
    unchanged("a damaged mod body (flag out of range)", bad, SMS_SNAPSHOT_MOD_REJECTED);
    bad = at5;
    bad[offset + 68 + 4 + 2] = 0xFF;   // a pad byte above the six buttons
    reseal(bad);
    unchanged("a damaged mod body (pad out of range)", bad, SMS_SNAPSHOT_MOD_REJECTED);
  }
  for (std::size_t cut : {at5.size() - 1, at5.size() - 40, std::size_t{200}}) {
    const std::vector<std::uint8_t> shorter(at5.begin(), at5.begin() + static_cast<std::ptrdiff_t>(cut));
    const std::vector<std::uint8_t> before = r.capture();
    check(r.restore(shorter) != SMS_SNAPSHOT_OK && r.capture() == before, "a truncated state is refused");
  }
}

void fault_at_three(Mod &, const SegaModHost *h) {
  if (h->phase == SEGA_MOD_PHASE_FRAME_END && h->frame == 3) h->write(SEGA_SMS_SPACE_Z80_WORK_RAM, 0x100, 1, 1);
}

void test_reset_restarts_the_witness() {
  Rig r;
  check(r.attach(kStateful).empty(), "attach");
  std::uint8_t fresh[32], again[32];
  r.run(6);
  sms_mod_write_log_digest(r.sched.get(), fresh);
  r.run(9);
  r.mod.counter = 0;   // what the contract asks of a mod: initialise its own state in `reset`
  r.start();
  r.run(6);
  sms_mod_write_log_digest(r.sched.get(), again);
  check(std::memcmp(fresh, again, 32) == 0, "after a machine reset the witness equals a fresh session's: a recording since the reset replays");
}

void test_disabled_and_unavailable() {
  {
    Rig r;
    r.mod.extra = fault_at_three;
    check(r.attach(kStateful).empty(), "attach");
    r.run(2);
    const std::vector<std::uint8_t> before_fault = r.capture();
    r.run(8);
    check(r.sched->disabled == 1 && r.word() == 9 && std::string(r.sched->fault.text) == "E_MOD_FAULT cause=range mod=test phase=frame-end frame=3", "the fault discards that callback's writes and disables the mod");
    const std::vector<std::uint8_t> disabled = r.capture();
    check(r.restore(before_fault) == SMS_SNAPSHOT_OK && r.sched->disabled == 0, "a state from before the fault restores an enabled mod");
    r.run(8);
    check(r.capture() == disabled, "... which faults again at the same boundary: the identical state");
    check(r.restore(before_fault) == SMS_SNAPSHOT_OK && r.restore(disabled) == SMS_SNAPSHOT_OK && r.sched->disabled == 1 && std::string(r.sched->fault.text).find("frame=3") != std::string::npos,
          "the disabled flag and its diagnostic are state");
    const std::size_t calls = r.mod.trace.size();
    r.run(10);
    check(r.mod.trace.size() == calls, "a restored disabled mod stays inert");
    const std::vector<std::uint8_t> inert = r.capture();
    r.mod.fail_load = true;   // what a Luau VM that faulted answers: its fault is sticky
    check(r.restore(before_fault) == SMS_SNAPSHOT_MOD_REJECTED && r.capture() == inert && std::string(r.sched->fault.text) == "E_MOD_FAULT cause=range mod=test phase=frame-end frame=3",
          "a disabled mod that refuses a restore keeps its first diagnostic: machine and mod state unchanged");
    r.start();
    sms_psg_sync(r.psg.get(), 0);
    const std::vector<std::uint8_t> at_reset = r.capture();
    check(!at_reset.empty() && r.restore(at_reset) == SMS_SNAPSHOT_OK && r.capture() == at_reset && r.sched->disabled == 1,
          "a state taken after a reset, before the first frame, restores (the fault stays)");
  }
  {
    Rig r;
    check(r.attach(kReadWrite).empty(), "a guest-affecting mod without state.mod attaches");
    r.run(3);
    SmsSnapshotStatus st = SMS_SNAPSHOT_OK;
    check(r.capture(&st).empty() && st == SMS_SNAPSHOT_MOD_UNAVAILABLE, "... but never yields a state that would silently drop its effect");
  }
}

void draw(Mod &m, const SegaModHost *h) {
  const auto *f = static_cast<const SegaMasterSystemFeatures *>(h->platform_features);
  if (h->phase != SEGA_MOD_PHASE_RENDER) return;
  if (m.counter++ == 2) { f->render->scroll(4, 0); f->render->overlay_rect(0, 0, 2, 2, 0xFFFFFFFFU); h->write(SEGA_SMS_SPACE_Z80_WORK_RAM, 0x10, 1, 1); return; }
  f->render->scroll(1, 0);
}

void test_render_is_transactional() {
  Rig r;
  r.mod.extra = draw;
  check(r.attach(cap(SEGA_MOD_CAP_OVERLAY_RGBA) | cap(SEGA_MOD_CAP_PRESENT_TRANSFORM) | kReadWrite, bit(SEGA_MOD_PHASE_RENDER)).empty(), "attach");
  r.run(2);
  const SmsModPresentation *p = sms_mod_scheduler_presentation(r.sched.get());
  check(p != nullptr && p->dx == 1, "a successful render commits its presentation");
  r.run(3);
  check(r.sched->disabled == 1 && sms_mod_scheduler_presentation(r.sched.get()) == nullptr && r.word() == 0 && std::string(r.sched->fault.text) == "E_MOD_FAULT cause=phase mod=test phase=render frame=2",
        "a render that tries to mutate guest memory faults deterministically: no write, and nothing it drew is presented");
}

}  // namespace

int main() {
  test_phases_input_and_reset();
  test_cart_ram_and_negotiation();
  test_state_and_rewind();
  test_reset_restarts_the_witness();
  test_disabled_and_unavailable();
  test_render_is_transactional();
  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("sms_mod_scheduler_tests: ok\n");
  return 0;
}
