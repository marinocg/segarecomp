// SEG-146-T006: the Genesis adapter of the shared session recorder (platforms/genesis/viewer/genesis_session.h).
// Project-authored synthetic machine only (no ROM): a program that folds the pad the guest reads into its state on every dispatch, so any difference in WHEN a pad
// becomes visible changes the final state. Recording with one host slicing and mid-frame host polls must replay identically with another slicing and no host input.
#include "frame_capture.h"
#include "genesis_session.h"
#include "runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>
#include <string>

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
const char *kOther = "2222222222222222222222222222222222222222222222222222222222222222";

// The guest reads the pad exactly as the controller I/O does: the mod override when active, else the physical pad.
GenesisControlTransfer folding_dispatch(GenesisRuntime *runtime) {
  const uint8_t p1 = runtime->mod_pad_active != 0U ? runtime->mod_pad1 : runtime->pad1;
  const uint8_t p2 = runtime->mod_pad_active != 0U ? runtime->mod_pad2 : runtime->pad2;
  runtime->d[1] = runtime->d[1] * 31U + p1 + 7U * p2;
  runtime->work_ram[runtime->d[0] % 4096U] ^= static_cast<uint8_t>(p1 + p2);
  runtime->d[0] += 1U;
  return genesis_runtime_retire_m68k_instruction(runtime, 640U, runtime->pc);
}

void setup(GenesisRuntime &r) {
  std::memset(static_cast<void *>(&r), 0, sizeof(r));
  r.pc = 0x200U;
  r.sr = 0x2700U;
  uint16_t *regs = r.devices.vdp.registers;
  regs[1] = 0x44; regs[2] = 0x30; regs[4] = 0x04; regs[5] = 0x28; regs[11] = 0x04; regs[12] = 0x81; regs[16] = 0x01;
}

// Runs until `frames` guest frames elapsed. host_pads: when true the host changes the physical pads every slice (the viewer's behaviour), at a rate unrelated to frames.
void run_frames(GenesisRuntime &r, GenesisSession *session, uint32_t slice, uint64_t frames, bool host_pads, uint32_t seed) {
  uint32_t n = seed;
  while (r.virtual_frame_count < frames && !(session != nullptr && session->done)) {
    if (host_pads) {
      n = n * 1103515245U + 12345U;
      r.pad1 = static_cast<uint8_t>((n >> 16) & ((n >> 24) & 1U ? 0xFFU : 0U));
      r.pad2 = static_cast<uint8_t>((n >> 8) & ((n >> 20) & 1U ? 0x3FU : 0U));
    }
    (void)genesis_runtime_run(&r, folding_dispatch, slice);
  }
}

std::string temp(const char *name) { return (std::string("segarecomp-seg146-") + name); }

std::string read_all(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The machine as the guest sees it: everything except the host-owned pad latches and hook pointers (they are not machine state).
std::vector<unsigned char> machine_bytes(const GenesisRuntime &r) {
  std::vector<unsigned char> bytes(sizeof r);
  std::memcpy(bytes.data(), &r, sizeof r);
  auto *c = reinterpret_cast<GenesisRuntime *>(bytes.data());
  c->pad1 = c->pad2 = c->mod_pad_active = c->mod_pad1 = c->mod_pad2 = c->host_yield_request = 0U;
  c->frame_hooks = nullptr;
  return bytes;
}

// Records `frames` guest frames with mid-frame host pads at `slice`, then stops ON the next boundary ("Stop recording"). Returns the machine exactly where the recording ended.
std::vector<unsigned char> record_session(const std::string &file, uint32_t slice, uint64_t frames, uint32_t seed, uint64_t *end_frame, bool host_pads = true) {
  char error[320];
  set_env("SEGARECOMP_ROM_SHA256", kImage);
  set_env("SEGARECOMP_SAVE_VARIANT", nullptr);
  set_env("SEGARECOMP_REPLAY", nullptr);
  set_env("SEGARECOMP_RECORD", file.c_str());
  auto rec = std::make_unique<GenesisRuntime>();
  setup(*rec);
  GenesisSession s;
  check(genesis_session_from_environment(&s, rec.get(), nullptr, error, sizeof error) == 1, "record session installs");
  uint32_t n = seed;
  bool requested = false;
  while (!s.done) {
    if (host_pads) {
      n = n * 1103515245U + 12345U;
      rec->pad1 = static_cast<uint8_t>((n >> 16) & ((n >> 24) & 1U ? 0xFFU : 0U));
      rec->pad2 = static_cast<uint8_t>((n >> 8) & ((n >> 20) & 1U ? 0x3FU : 0U));
    }
    if (!requested && rec->virtual_frame_count >= frames) { requested = true; check(genesis_session_request_end(&s) == 0, "stop recording is accepted"); }
    (void)genesis_runtime_run(rec.get(), folding_dispatch, slice);
  }
  check(!s.failed && std::strstr(s.message, "recording saved") != nullptr, "recording finishes with a summary");
  *end_frame = rec->virtual_frame_count;
  check(s.frames == *end_frame, "the recording ended on the boundary it reports");
  auto bytes = machine_bytes(*rec);
  genesis_session_finish(&s);
  check(rec->frame_hooks == nullptr && rec->mod_pad_active == 0U, "the adapter removes itself");
  return bytes;
}

// Replays at `slice`; returns the machine at the moment the runner yielded.
std::vector<unsigned char> replay_session(const std::string &file, uint32_t slice, bool host_pads, uint64_t *frame, uint32_t *checked = nullptr) {
  char error[320];
  set_env("SEGARECOMP_RECORD", nullptr);
  set_env("SEGARECOMP_REPLAY", file.c_str());
  auto rp = std::make_unique<GenesisRuntime>();
  setup(*rp);
  GenesisSession t;
  check(genesis_session_from_environment(&t, rp.get(), nullptr, error, sizeof error) == 1, "replay session installs");
  uint32_t n = 77U;
  uint64_t dispatches = 0U;
  while (!t.done && dispatches < 100000000U) {
    if (host_pads) { n = n * 1103515245U + 12345U; rp->pad1 = static_cast<uint8_t>(n >> 16); rp->pad2 = static_cast<uint8_t>(n >> 8); }
    const auto transfer = genesis_runtime_run(rp.get(), folding_dispatch, slice);
    dispatches += transfer.runner_dispatch_count;
  }
  check(t.done && !t.failed && t.ended_cleanly, (std::string("replay finishes without divergence (") + t.message + ")").c_str());
  check(t.frames == t.replay.end_frame && rp->virtual_frame_count == t.replay.end_frame, "the replay stopped exactly on the end boundary: no frame and no instruction beyond it ran");
  if (frame != nullptr) *frame = rp->virtual_frame_count;
  if (checked != nullptr) *checked = t.checkpoints_checked;
  auto bytes = machine_bytes(*rp);
  genesis_session_finish(&t);
  return bytes;
}

void record_then_replay_reproduces_the_whole_machine_at_the_end_boundary() {
  const std::string file = temp("session.rec");
  uint64_t end_frame = 0U;
  const auto recorded = record_session(file, 977U, 1300U, 1U, &end_frame);
  check(end_frame >= 1300U && end_frame < 1320U, "the recording ended at the first boundary after the stop request");
  const std::string text = read_all(file);
  check(text.find("segarecomp-session 1") == 0 && text.find("digest basic") != std::string::npos && text.find("\nend ") != std::string::npos &&
            text.find("checkpoint 600 ") != std::string::npos && text.find("checkpoint 1200 ") != std::string::npos,
        "header, digest profile, checkpoints and an end marker");
  for (const uint32_t slice : {1U, 500U, 7919U, 65536U}) {
    for (const bool host_pads : {false, true}) {
      uint64_t frame = 0U;
      uint32_t checked = 0U;
      const auto replayed = replay_session(file, slice, host_pads, &frame, &checked);
      check(frame == end_frame, "the replay ends at the recorded frame");
      check(checked >= 3U, "every checkpoint was compared");
      check(replayed == recorded, ("the machine at the end boundary is byte-identical to the recorded run's, at slice " + std::to_string(slice) + (host_pads ? " with host pads" : "")).c_str());
    }
  }
  std::remove(file.c_str());
}

void identity_and_flags() {
  const std::string file = temp("ident.rec");
  char error[320];
  set_env("SEGARECOMP_ROM_SHA256", kImage);
  set_env("SEGARECOMP_RECORD", file.c_str());
  set_env("SEGARECOMP_REPLAY", nullptr);
  GenesisRuntime rec;
  setup(rec);
  GenesisSession s;
  check(genesis_session_from_environment(&s, &rec, nullptr, error, sizeof error) == 1, "record installs");
  run_frames(rec, &s, 500U, 30U, true, 3U);
  genesis_session_finish(&s);

  set_env("SEGARECOMP_RECORD", nullptr);
  set_env("SEGARECOMP_REPLAY", file.c_str());
  set_env("SEGARECOMP_ROM_SHA256", kOther);
  GenesisRuntime r;
  setup(r);
  GenesisSession t;
  check(genesis_session_from_environment(&t, &r, nullptr, error, sizeof error) == -1 && std::strstr(error, "--allow-variant") != nullptr, "a different effective image is refused");
  check(r.frame_hooks == nullptr, "a refused session installs nothing");
  set_env("SEGARECOMP_ALLOW_VARIANT", "1");
  check(genesis_session_from_environment(&t, &r, nullptr, error, sizeof error) == 1 && t.replay.variant_mismatch, "--allow-variant admits it");
  run_frames(r, &t, 700U, 100000U, false, 0U);
  check(t.done && !t.failed, "a variant replay is not judged by checkpoints");
  genesis_session_finish(&t);
  set_env("SEGARECOMP_ALLOW_VARIANT", nullptr);

  set_env("SEGARECOMP_ROM_SHA256", nullptr);
  GenesisRuntime n;
  setup(n);
  GenesisSession u;
  check(genesis_session_from_environment(&u, &n, nullptr, error, sizeof error) == -1 && std::strstr(error, "no verified identity") != nullptr, "no identity, no session");
  set_env("SEGARECOMP_ROM_SHA256", kImage);
  set_env("SEGARECOMP_RECORD", file.c_str());
  GenesisSession both;
  check(genesis_session_from_environment(&both, &n, nullptr, error, sizeof error) == -1 && std::strstr(error, "cannot be used together") != nullptr, "--record and --replay are exclusive");
  set_env("SEGARECOMP_RECORD", nullptr);
  std::remove(file.c_str());
}

void divergence_is_reported_at_the_checkpoint() {
  const std::string file = temp("diverge.rec");
  char error[320];
  set_env("SEGARECOMP_ROM_SHA256", kImage);
  set_env("SEGARECOMP_RECORD", file.c_str());
  set_env("SEGARECOMP_REPLAY", nullptr);
  GenesisRuntime rec;
  setup(rec);
  GenesisSession s;
  check(genesis_session_from_environment(&s, &rec, nullptr, error, sizeof error) == 1, "record installs");
  run_frames(rec, &s, 4096U, 700U, true, 11U);
  genesis_session_finish(&s);
  set_env("SEGARECOMP_RECORD", nullptr);
  set_env("SEGARECOMP_REPLAY", file.c_str());
  GenesisRuntime rp;
  setup(rp);
  rp.d[1] = 1234U;   // a different program state: the machine is not the recorded one
  rp.work_ram[5000] = 1U;
  GenesisSession t;
  check(genesis_session_from_environment(&t, &rp, nullptr, error, sizeof error) == 1, "replay installs");
  run_frames(rp, &t, 4096U, 100000U, false, 0U);
  check(t.done && t.failed && std::strstr(t.message, "diverged") != nullptr && std::strstr(t.message, "frame 0 ") != nullptr, "a machine that is not the recorded one is reported at the power-on checkpoint");
  genesis_session_finish(&t);
  std::remove(file.c_str());
}

void an_unfinished_recording_is_refused() {
  const std::string file = temp("unfinished.rec");
  char error[320];
  set_env("SEGARECOMP_ROM_SHA256", kImage);
  set_env("SEGARECOMP_RECORD", file.c_str());
  set_env("SEGARECOMP_REPLAY", nullptr);
  GenesisRuntime rec;
  setup(rec);
  GenesisSession s;
  check(genesis_session_from_environment(&s, &rec, nullptr, error, sizeof error) == 1, "record installs");
  run_frames(rec, &s, 4096U, 10U, true, 2U);
  // Not finished: the file is a valid prefix (streamed) but has no end marker.
  set_env("SEGARECOMP_RECORD", nullptr);
  set_env("SEGARECOMP_REPLAY", file.c_str());
  GenesisRuntime rp;
  setup(rp);
  GenesisSession t;
  check(genesis_session_from_environment(&t, &rp, nullptr, error, sizeof error) == -1 && std::strstr(error, "no end marker") != nullptr, "no end marker, no replay");
  genesis_session_finish(&s);
  std::remove(file.c_str());
}

// The headless path: frame_capture.c's environment driver replays the SAME file and runs to the end marker even though the capture window is one frame.
void headless_replay_runs_to_the_end_marker() {
  const std::string file = temp("headless.rec");
  const std::string dir = temp("headless-frames");
  char error[320];
  set_env("SEGARECOMP_ROM_SHA256", kImage);
  set_env("SEGARECOMP_RECORD", file.c_str());
  set_env("SEGARECOMP_REPLAY", nullptr);
  GenesisRuntime rec;
  setup(rec);
  GenesisSession s;
  check(genesis_session_from_environment(&s, &rec, nullptr, error, sizeof error) == 1, "record installs");
  run_frames(rec, &s, 600U, 40U, true, 6U);
  genesis_session_finish(&s);
  const uint64_t end_frame = rec.virtual_frame_count;
  set_env("SEGARECOMP_RECORD", nullptr);
  set_env("SEGARECOMP_REPLAY", file.c_str());
  set_env("SEGARECOMP_CAPTURE_DIR", dir.c_str());
  set_env("SEGARECOMP_CAPTURE_FIRST", "1");
  set_env("SEGARECOMP_CAPTURE_COUNT", "1");
  set_env("SEGARECOMP_CAPTURE_SLICE", "700");
  std::filesystem::create_directories(dir);
  GenesisRuntime rp;
  setup(rp);
  const GenesisControlTransfer t = genesis_frame_capture_run_from_environment(&rp, folding_dispatch, 4000000U);
  check(t.kind == GENESIS_RUNNER_RESOURCE_LIMIT, "the capture window completed");
  check(rp.virtual_frame_count >= end_frame && rp.virtual_frame_count < end_frame + 20U, "the replay ran to the recording's end, past the one-frame capture window, and stopped there");
  set_env("SEGARECOMP_REPLAY", nullptr);
  set_env("SEGARECOMP_CAPTURE_DIR", nullptr);
  std::filesystem::remove_all(dir);
  std::remove(file.c_str());
}

void a_wrapped_scheduler_hook_still_runs() {
  struct Counter { int starts = 0, frame_ends = 0, afters = 0; } counter;
  GenesisFrameHooks inner{};
  inner.ctx = &counter;
  inner.start = [](void *c, GenesisRuntime *) { ++static_cast<Counter *>(c)->starts; };
  inner.frame_end = [](void *c, GenesisRuntime *) { ++static_cast<Counter *>(c)->frame_ends; };
  inner.after_snapshot = [](void *c, GenesisRuntime *) { ++static_cast<Counter *>(c)->afters; };
  const std::string file = temp("wrap.rec");
  char error[320];
  set_env("SEGARECOMP_ROM_SHA256", kImage);
  set_env("SEGARECOMP_RECORD", file.c_str());
  set_env("SEGARECOMP_REPLAY", nullptr);
  GenesisRuntime rec;
  setup(rec);
  rec.frame_hooks = &inner;
  GenesisSession s;
  check(genesis_session_from_environment(&s, &rec, nullptr, error, sizeof error) == 1, "wraps an installed scheduler");
  run_frames(rec, &s, 2000U, 12U, true, 4U);
  genesis_session_finish(&s);
  check(rec.frame_hooks == &inner, "the original hooks are restored");
  check(counter.starts >= 1 && counter.frame_ends >= 11 && counter.afters >= 11, "the wrapped hooks keep running");
  set_env("SEGARECOMP_RECORD", nullptr);
  std::remove(file.c_str());
}
}  // namespace

int main() {
  record_then_replay_reproduces_the_whole_machine_at_the_end_boundary();
  identity_and_flags();
  divergence_is_reported_at_the_checkpoint();
  an_unfinished_recording_is_refused();
  headless_replay_runs_to_the_end_marker();
  a_wrapped_scheduler_hook_still_runs();
  if (failures == 0) std::puts("genesis_session_tests: ok");
  return failures == 0 ? 0 : 1;
}
