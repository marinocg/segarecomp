// SEG-146-T006: the whole-machine checkpoint digest of a session (genesis_session_machine.h) and the session adapter wrapped around a REAL mod scheduler that intercepts input.
// Project-authored synthetic machine only (no ROM).
#include "genesis_session_machine.h"
#include "genesis_snapshot.h"
#include "mod_scheduler.h"
#include "runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool value, const char *message) {
  if (!value) {
    std::printf("FAIL: %s\n", message);
    ++failures;
  }
}
void set_env(const char *name, const char *value) {
#ifdef _WIN32
  _putenv_s(name, value != nullptr ? value : "");
#else
  if (value != nullptr) setenv(name, value, 1); else unsetenv(name);
#endif
}
const char *kImage = "1111111111111111111111111111111111111111111111111111111111111111";

// The guest reads controller port 1 through the real controller I/O route on every dispatch (so the mod's intercepted pad is what it sees) and folds it into its state.
GenesisControlTransfer program(GenesisRuntime *rt) {
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
  rt->d[1] = rt->d[1] * 31U + value;
  rt->d[3] += (value & 1U) == 0U ? 1U : 0U;                      // reads where the guest saw UP pressed (port bit 0 is active low)
  rt->work_ram[rt->d[0] % 2048U] ^= static_cast<uint8_t>(value);
  rt->d[0] += 1U;
  return genesis_runtime_retire_m68k_instruction(rt, 640U, rt->pc);
}

void setup(GenesisRuntime &r) {
  std::memset(static_cast<void *>(&r), 0, sizeof(r));
  r.pc = 0x200U;
  r.sr = 0x2700U;
  uint16_t *regs = r.devices.vdp.registers;
  regs[1] = 0x44; regs[2] = 0x30; regs[4] = 0x04; regs[5] = 0x28; regs[11] = 0x04; regs[12] = 0x81; regs[16] = 0x01;
}

// A native mod that intercepts input: it presses UP on every frame (the physical pad is untouched).
void intercept_phase(void *, const SegaModHost *h) {
  uint32_t v = 0;
  if (h->phase == SEGA_MOD_PHASE_INPUT) {
    h->input_get(1, &v);
    h->input_set(1, v | GENESIS_PAD_UP);
  }
}

struct Machine {
  std::unique_ptr<GenesisRuntime> rt = std::make_unique<GenesisRuntime>();
  std::unique_ptr<GenesisModScheduler> sched = std::make_unique<GenesisModScheduler>();
  SegaModProvider provider{};
  bool mod = false;
  GenesisSessionMachineDigest machine_digest{};
  GenesisSessionDigest digest{};
  GenesisSession session{};
  explicit Machine(bool with_mod) : mod(with_mod) {
    setup(*rt);
    if (mod) {
      provider.size = sizeof provider;
      provider.abi_major = SEGA_MOD_ABI_MAJOR;
      provider.platform = SEGA_MOD_PLATFORM_GENESIS;
      provider.name = "intercept";
      provider.implementation_version = 1;
      provider.phase = intercept_phase;
      GenesisModConfig c{};
      c.mod_id = "intercept";
      c.provider = &provider;
      c.required[0] = (UINT64_C(1) << SEGA_MOD_CAP_INPUT_READ) | (UINT64_C(1) << SEGA_MOD_CAP_INPUT_INTERCEPT);
      c.phase_mask = 1U << SEGA_MOD_PHASE_INPUT;
      const char *why = "";
      check(genesis_mod_scheduler_attach(sched.get(), rt.get(), &c, &why) == 0, "the intercepting mod attaches");
    }
    GenesisSnapshotMachine m{};
    m.runtime = rt.get();
    genesis_session_machine_digest_init(&digest, &machine_digest, &m, nullptr);
  }
  ~Machine() { if (mod) genesis_mod_scheduler_detach(sched.get()); }
  std::vector<uint8_t> digest_now() {
    std::vector<uint8_t> d(32);
    check(genesis_session_machine_digest_fn(&machine_digest, rt.get(), d.data()) == 0, "the machine can be digested");
    return d;
  }
};

std::string read_all(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Runs until the session is done; `stop_at` asks a recording to end at the next boundary after that many frames.
void drive(Machine &m, uint32_t slice, uint64_t stop_at, bool host_pads, uint32_t seed) {
  uint32_t n = seed;
  bool requested = false;
  for (uint64_t guard = 0; !m.session.done && guard < 100000000U; ++guard) {
    if (host_pads) { n = n * 1103515245U + 12345U; m.rt->pad1 = static_cast<uint8_t>((n >> 16) & ((n >> 24) & 1U ? 0xFFU : 0U)); m.rt->pad2 = static_cast<uint8_t>(n >> 8); }
    if (m.session.mode == GENESIS_SESSION_RECORD && !requested && m.rt->virtual_frame_count >= stop_at) { requested = true; (void)genesis_session_request_end(&m.session); }
    (void)genesis_runtime_run(m.rt.get(), program, slice);
  }
}

void whole_machine_checkpoints_cover_every_device(bool with_mod) {
  const std::string file = std::string("segarecomp-seg146-machine-") + (with_mod ? "mod" : "plain") + ".rec";
  char error[320];
  set_env("SEGARECOMP_ROM_SHA256", kImage);
  set_env("SEGARECOMP_SAVE_VARIANT", nullptr);
  set_env("SEGARECOMP_REPLAY", nullptr);
  set_env("SEGARECOMP_RECORD", file.c_str());
  Machine rec(with_mod);
  check(genesis_session_from_environment(&rec.session, rec.rt.get(), &rec.digest, error, sizeof error) == 1, "record installs");
  drive(rec, 611U, 700U, true, 5U);
  check(rec.session.done && !rec.session.failed, "recording ended");
  const uint64_t end_frame = rec.rt->virtual_frame_count;
  const auto recorded_digest = rec.digest_now();
  const uint32_t recorded_d1 = rec.rt->d[1], recorded_seen = rec.rt->d[3], recorded_reads = rec.rt->d[0];
  genesis_session_finish(&rec.session);
  const std::string text = read_all(file);
  check(text.find("digest machine-") != std::string::npos && text.find("digest basic") == std::string::npos, "the recording names the whole-machine digest profile");
  check(text.find("checkpoint 0 ") != std::string::npos && text.find("checkpoint 600 ") != std::string::npos, "power-on and interval checkpoints were written");
  check(text.find("checkpoint " + std::to_string(end_frame) + " ") != std::string::npos, "and one on the end marker's own frame");
  if (with_mod) {
    // The recording stores the PHYSICAL pad: the mod forces UP on the guest's copy only, so at least one stored mask has UP released while the guest saw it pressed every frame.
    bool released_in_file = false;
    size_t at = 0;
    while ((at = text.find("\n", at)) != std::string::npos) {
      ++at;
      unsigned long frame = 0; unsigned player = 0, mask = 0;
      if (std::sscanf(text.c_str() + at, "%lu %u %u", &frame, &player, &mask) == 3 && player == 1U && (mask & GENESIS_PAD_UP) == 0U) released_in_file = true;
    }
    check(released_in_file, "the recording keeps the physical pad, not the mod's intercepted one");
    check(recorded_seen > 0U && recorded_seen + 4U > recorded_reads / 2U, "the guest saw the intercepted pad: UP pressed on (almost) every read, though the file holds frames with UP released");
  }

  for (const uint32_t slice : {1U, 997U, 65536U}) {
    set_env("SEGARECOMP_RECORD", nullptr);
    set_env("SEGARECOMP_REPLAY", file.c_str());
    Machine rp(with_mod);
    check(genesis_session_from_environment(&rp.session, rp.rt.get(), &rp.digest, error, sizeof error) == 1, "replay installs");
    drive(rp, slice, 0U, slice == 997U, 9U);
    if (!(rp.session.done && !rp.session.failed && rp.session.ended_cleanly)) std::printf("  mod=%d slice=%u: %s (frame %llu)\n", with_mod, slice, rp.session.message, (unsigned long long)rp.rt->virtual_frame_count);
    check(rp.session.done && !rp.session.failed && rp.session.ended_cleanly, "the replay ends cleanly on its end marker");
    check(rp.rt->virtual_frame_count == end_frame, "and exactly on the recorded end boundary");
    check(rp.session.checkpoints_checked >= 2U, "checkpoints were compared");
    check(rp.session.verified && std::strstr(rp.session.message, "VERIFIED") != nullptr && rp.session.replay.compared == rp.session.replay.checkpoint_count, "the replay is VERIFIED: every checkpoint, the end marker's included, matched");
    check(rp.digest_now() == recorded_digest, "the whole-machine digest at the end boundary equals the recorded run's");
    check(rp.rt->d[1] == recorded_d1, "and so does the guest's own folded input history");
    genesis_session_finish(&rp.session);
  }

  // One flipped VRAM bit - invisible to work RAM, CPU and timing - is caught by the very first (power-on) checkpoint.
  set_env("SEGARECOMP_REPLAY", file.c_str());
  Machine bad(with_mod);
  bad.rt->devices.vdp.vram[1234] ^= 1U;
  check(genesis_session_from_environment(&bad.session, bad.rt.get(), &bad.digest, error, sizeof error) == 1, "replay installs");
  drive(bad, 500U, 0U, false, 0U);
  check(bad.session.done && bad.session.failed && std::strstr(bad.session.message, "diverged from the recording at frame 0") != nullptr, "a VRAM difference is a divergence at the first checkpoint");
  genesis_session_finish(&bad.session);

  // The same recording replayed by a host without the whole-machine digest (the "basic" profile): it replays, is not judged by checkpoints, and says so.
  {
    Machine other(with_mod);
    set_env("SEGARECOMP_REPLAY", file.c_str());
    check(genesis_session_from_environment(&other.session, other.rt.get(), nullptr, error, sizeof error) == 1, "a host with another digest profile still replays");
    check(other.session.replay.variant_mismatch && std::strstr(other.session.replay.note, "machine digest") != nullptr, "and is told the checkpoints are not enforced");
    drive(other, 700U, 0U, false, 0U);
    check(other.session.done && !other.session.failed && other.session.ended_cleanly, "the replay completes");
    check(!other.session.verified && other.session.checkpoints_checked == 0U && std::strstr(other.session.message, "NOT VERIFIED") != nullptr, "but it is never reported as verified: no checkpoint was compared");
    genesis_session_finish(&other.session);
  }
  set_env("SEGARECOMP_REPLAY", nullptr);
  std::remove(file.c_str());
}
}  // namespace

int main() {
  whole_machine_checkpoints_cover_every_device(false);
  whole_machine_checkpoints_cover_every_device(true);
  if (failures == 0) std::puts("genesis_session_machine_tests: ok");
  return failures == 0 ? 0 : 1;
}
