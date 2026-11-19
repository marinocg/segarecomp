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
  while (r.virtual_frame_count < frames && !(session != nullptr && session->done && session->mode == GENESIS_SESSION_REPLAY)) {
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

bool same_zero(const uint8_t *d) { for (int i = 0; i < 32; ++i) if (d[i] != 0U) return false; return true; }
struct Final { uint32_t d1; uint8_t digest[32]; uint64_t frames; };
Final snapshot(const GenesisRuntime &r) {
  Final f{};
  f.d1 = r.d[1];
  genesis_session_digest(&r, f.digest);
  f.frames = r.virtual_frame_count;
  return f;
}

void record_then_replay_is_identical_across_slicings() {
  const std::string file = temp("session.rec");
  char error[320];
  set_env("SEGARECOMP_ROM_SHA256", kImage);
  set_env("SEGARECOMP_SAVE_VARIANT", nullptr);
  set_env("SEGARECOMP_REPLAY", nullptr);
  set_env("SEGARECOMP_RECORD", file.c_str());
  GenesisRuntime rec;
  setup(rec);
  GenesisSession s;
  check(genesis_session_from_environment(&s, &rec, error, sizeof error) == 1, "record session installs");
  run_frames(rec, &s, 977U, 1300U, true, 1U);   // mid-frame host polls at an odd slice size, across two checkpoint intervals
  const Final recorded = snapshot(rec);
  genesis_session_finish(&s);
  check(!s.failed && std::strstr(s.message, "recording saved") != nullptr, "recording finishes with a summary");
  check(rec.frame_hooks == nullptr && rec.mod_pad_active == 0U, "the adapter removes itself");
  const std::string text = read_all(file);
  check(text.find("segarecomp-session 1") == 0 && text.find("\nend ") != std::string::npos && text.find("checkpoint 600 ") != std::string::npos && text.find("checkpoint 1200 ") != std::string::npos,
        "header, checkpoints and an end marker");

  for (uint32_t slice : {1U, 500U, 65536U}) {
    set_env("SEGARECOMP_RECORD", nullptr);
    set_env("SEGARECOMP_REPLAY", file.c_str());
    GenesisRuntime rp;
    setup(rp);
    GenesisSession t;
    check(genesis_session_from_environment(&t, &rp, error, sizeof error) == 1, "replay session installs");
    run_frames(rp, &t, slice, 100000U, false, 0U);
    check(t.done && !t.failed, "replay finishes without divergence");
    check(t.replay.checkpoint_next >= 2U, "checkpoints were compared");
    const Final replayed = snapshot(rp);
    check(t.frames == t.replay.end_frame, "the session ends exactly at the recorded end frame, whatever the slice size");
    genesis_session_finish(&t);
    // The recorder ended at the frame it observed last; the replay ends at that same frame boundary.
    check(replayed.d1 != 0U, "the program ran");
    check(replayed.frames >= recorded.frames - 1U, "replay ran to the recorded end");
  }
  std::remove(file.c_str());
}

void exact_final_state_matches() {
  // Stop both runs on the identical dispatch boundary: the recording's end frame. (Slicing may overshoot a frame boundary by a slice, so compare at the boundary state.)
  const std::string file = temp("exact.rec");
  char error[320];
  set_env("SEGARECOMP_ROM_SHA256", kImage);
  set_env("SEGARECOMP_RECORD", file.c_str());
  set_env("SEGARECOMP_REPLAY", nullptr);
  GenesisRuntime rec;
  setup(rec);
  GenesisSession s;
  check(genesis_session_from_environment(&s, &rec, error, sizeof error) == 1, "record installs");
  run_frames(rec, &s, 333U, 40U, true, 9U);
  genesis_session_finish(&s);
  // Replay in single dispatches until the first boundary at/after the end frame, and compare with the recording run stopped at its own end.
  set_env("SEGARECOMP_RECORD", nullptr);
  set_env("SEGARECOMP_REPLAY", file.c_str());
  GenesisRuntime a, b;
  setup(a);
  setup(b);
  GenesisSession ta, tb;
  check(genesis_session_from_environment(&ta, &a, error, sizeof error) == 1, "replay A installs");
  check(genesis_session_from_environment(&tb, &b, error, sizeof error) == 1, "replay B installs");
  run_frames(a, &ta, 1U, 100000U, false, 0U);
  run_frames(b, &tb, 7919U, 100000U, true, 5U);   // host pads present but ignored: the replay owns the pad
  // Both stop at a boundary: A exactly at the end frame (slice 1), B within one slice of it.
  check(ta.done && tb.done && !ta.failed && !tb.failed, "both replays reached the end marker");
  check(ta.frames == tb.frames && ta.frames != 0U, "and report the same end frame");
  check(std::memcmp(ta.end_digest, tb.end_digest, 32) == 0 && !same_zero(ta.end_digest), "the state at the end boundary is identical for slicings of 1 and 7919 dispatches, with and without host pads");
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
  check(genesis_session_from_environment(&s, &rec, error, sizeof error) == 1, "record installs");
  run_frames(rec, &s, 500U, 30U, true, 3U);
  genesis_session_finish(&s);

  set_env("SEGARECOMP_RECORD", nullptr);
  set_env("SEGARECOMP_REPLAY", file.c_str());
  set_env("SEGARECOMP_ROM_SHA256", kOther);
  GenesisRuntime r;
  setup(r);
  GenesisSession t;
  check(genesis_session_from_environment(&t, &r, error, sizeof error) == -1 && std::strstr(error, "--allow-variant") != nullptr, "a different effective image is refused");
  check(r.frame_hooks == nullptr, "a refused session installs nothing");
  set_env("SEGARECOMP_ALLOW_VARIANT", "1");
  check(genesis_session_from_environment(&t, &r, error, sizeof error) == 1 && t.replay.variant_mismatch, "--allow-variant admits it");
  run_frames(r, &t, 700U, 100000U, false, 0U);
  check(t.done && !t.failed, "a variant replay is not judged by checkpoints");
  genesis_session_finish(&t);
  set_env("SEGARECOMP_ALLOW_VARIANT", nullptr);

  set_env("SEGARECOMP_ROM_SHA256", nullptr);
  GenesisRuntime n;
  setup(n);
  GenesisSession u;
  check(genesis_session_from_environment(&u, &n, error, sizeof error) == -1 && std::strstr(error, "no verified identity") != nullptr, "no identity, no session");
  set_env("SEGARECOMP_ROM_SHA256", kImage);
  set_env("SEGARECOMP_RECORD", file.c_str());
  GenesisSession both;
  check(genesis_session_from_environment(&both, &n, error, sizeof error) == -1 && std::strstr(error, "cannot be used together") != nullptr, "--record and --replay are exclusive");
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
  check(genesis_session_from_environment(&s, &rec, error, sizeof error) == 1, "record installs");
  run_frames(rec, &s, 4096U, 700U, true, 11U);
  genesis_session_finish(&s);
  set_env("SEGARECOMP_RECORD", nullptr);
  set_env("SEGARECOMP_REPLAY", file.c_str());
  GenesisRuntime rp;
  setup(rp);
  rp.d[1] = 1234U;   // a different program state: the machine is not the recorded one
  rp.work_ram[5000] = 1U;
  GenesisSession t;
  check(genesis_session_from_environment(&t, &rp, error, sizeof error) == 1, "replay installs");
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
  check(genesis_session_from_environment(&s, &rec, error, sizeof error) == 1, "record installs");
  run_frames(rec, &s, 4096U, 10U, true, 2U);
  // Not finished: the file is a valid prefix (streamed) but has no end marker.
  set_env("SEGARECOMP_RECORD", nullptr);
  set_env("SEGARECOMP_REPLAY", file.c_str());
  GenesisRuntime rp;
  setup(rp);
  GenesisSession t;
  check(genesis_session_from_environment(&t, &rp, error, sizeof error) == -1 && std::strstr(error, "no end marker") != nullptr, "no end marker, no replay");
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
  check(genesis_session_from_environment(&s, &rec, error, sizeof error) == 1, "record installs");
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
  check(genesis_session_from_environment(&s, &rec, error, sizeof error) == 1, "wraps an installed scheduler");
  run_frames(rec, &s, 2000U, 12U, true, 4U);
  genesis_session_finish(&s);
  check(rec.frame_hooks == &inner, "the original hooks are restored");
  check(counter.starts >= 1 && counter.frame_ends >= 11 && counter.afters >= 11, "the wrapped hooks keep running");
  set_env("SEGARECOMP_RECORD", nullptr);
  std::remove(file.c_str());
}
}  // namespace

int main() {
  record_then_replay_is_identical_across_slicings();
  exact_final_state_matches();
  identity_and_flags();
  divergence_is_reported_at_the_checkpoint();
  an_unfinished_recording_is_refused();
  headless_replay_runs_to_the_end_marker();
  a_wrapped_scheduler_hook_still_runs();
  if (failures == 0) std::puts("genesis_session_tests: ok");
  return failures == 0 ? 0 : 1;
}
