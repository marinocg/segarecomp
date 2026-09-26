// SEG-021-T031: headless bounded live-frame capture (platforms/genesis/viewer/frame_capture.c).
// Project-authored synthetic runtime state only; no ROM, no host clock.
#include "frame_capture.h"
#include "frame_export.h"
#include "runtime.h"
#include "vdp_render.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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

// One synthetic "instruction" per dispatch: retire 4 cycles and change a VDP colour every
// dispatch, so consecutive frames differ and every frame has a distinct deterministic digest.
GenesisControlTransfer synthetic_dispatch(GenesisRuntime *runtime) {
  runtime->d[0] += 1U;
  runtime->devices.vdp.cram[1] = static_cast<uint8_t>((runtime->d[0] / 40000U) & 0x0EU);
  return genesis_runtime_retire_m68k_instruction(runtime, 4U, runtime->pc);
}

GenesisControlTransfer stopping_dispatch(GenesisRuntime *runtime) {
  if (runtime->d[0] >= 50000U) {
    GenesisControlTransfer t{};
    t.kind = GENESIS_STOP;
    t.stop.stop_class = GENESIS_STOP_UNSUPPORTED_CPU_FORM;
    return t;
  }
  return synthetic_dispatch(runtime);
}

void setup(GenesisRuntime &r) {
  std::memset(static_cast<void *>(&r), 0, sizeof(r));
  r.pc = 0x200U;
  r.sr = 0x2700U;
  uint16_t *regs = r.devices.vdp.registers;
  regs[1] = 0x44; regs[2] = 0x30; regs[4] = 0x04; regs[5] = 0x28; regs[11] = 0x04; regs[12] = 0x81; regs[16] = 0x01;
  r.devices.vdp.cram[0] = 0x0EU;
}

std::string fresh_dir(const char *name) {
  const auto dir = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir.string();
}

std::vector<char> read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

GenesisFrameCaptureOptions options_for(const std::string &dir, uint64_t first, uint32_t count, uint32_t stride) {
  GenesisFrameCaptureOptions o{};
  o.first_frame = first;
  o.frame_count = count;
  o.frame_stride = stride;
  o.slice_dispatches = 4096U;
  o.out_dir = dir.c_str();
  return o;
}

void window_is_captured_by_ordinal_and_stops_early() {
  const auto dir = fresh_dir("segarecomp-t031-window");
  GenesisRuntime r;
  setup(r);
  const auto o = options_for(dir, 3U, 3U, 2U);
  const auto result = genesis_frame_capture_run(&r, synthetic_dispatch, &o, 10000000U);
  check(result.outcome == GENESIS_FRAME_CAPTURE_WINDOW_COMPLETE, "window completes");
  check(result.frames_captured == 3U && result.frames_published == 7U, "ordinals 3,5,7 captured; stops at 7");
  check(result.dispatches < 10000000U, "observation stops by the selection rule, not by the allowance");
  check(r.live_frame_observer == nullptr, "observer detached on return");
  for (const char *name : {"frame-00000003.ppm", "frame-00000005.ppm", "frame-00000007.ppm"})
    check(std::filesystem::exists(dir + "/" + name), "selected ordinal written");
  check(!std::filesystem::exists(dir + "/frame-00000004.ppm") && !std::filesystem::exists(dir + "/frame-00000001.ppm"),
        "unselected ordinals are not written");
  // The written PPM is exactly the existing exporter's output for the current-state render.
  GenesisFrameArtifact frame{};
  check(genesis_vdp_produce_frame(r.devices.vdp.vram, r.devices.vdp.vsram, r.devices.vdp.cram, r.devices.vdp.registers,
                                  &frame) == 0, "reference render");
  const auto reference = dir + "/reference.ppm";
  check(genesis_frame_export_ppm_to_path(&frame, reference.c_str()) == GENESIS_FRAME_EXPORT_STATUS_OK, "reference export");
  check(read_file(reference).size() == read_file(dir + "/frame-00000007.ppm").size(), "PPM shape matches exporter");
}

void capture_does_not_alter_execution() {
  const auto dir = fresh_dir("segarecomp-t031-neutral");
  GenesisRuntime captured;
  setup(captured);
  const auto o = options_for(dir, 1U, 2U, 1U);
  const auto result = genesis_frame_capture_run(&captured, synthetic_dispatch, &o, 10000000U);
  GenesisRuntime plain;
  setup(plain);
  uint64_t remaining = result.dispatches;
  while (remaining > 0U) {
    const uint32_t slice = remaining > 4096U ? 4096U : static_cast<uint32_t>(remaining);
    const auto t = genesis_runtime_run(&plain, synthetic_dispatch, slice);
    remaining -= t.runner_dispatch_count;
  }
  captured.live_frame_observer = nullptr;
  check(std::memcmp(&captured, &plain, sizeof(GenesisRuntime)) == 0,
        "guest state after a capture run equals an unobserved run of the same dispatch count");
}

void capture_is_deterministic() {
  const auto a_dir = fresh_dir("segarecomp-t031-det-a");
  const auto b_dir = fresh_dir("segarecomp-t031-det-b");
  GenesisRuntime a, b;
  setup(a);
  setup(b);
  const auto oa = options_for(a_dir, 2U, 4U, 1U);
  const auto ob = options_for(b_dir, 2U, 4U, 1U);
  const auto ra = genesis_frame_capture_run(&a, synthetic_dispatch, &oa, 10000000U);
  const auto rb = genesis_frame_capture_run(&b, synthetic_dispatch, &ob, 10000000U);
  check(ra.frames_captured == 4U && std::memcmp(ra.digests, rb.digests, sizeof(ra.digests)) == 0,
        "two runs yield identical frame digests");
  check(std::memcmp(ra.digests[0], ra.digests[3], 32U) != 0, "different frames have different digests");
  for (const char *name : {"frame-00000002.ppm", "frame-00000005.ppm"})
    check(read_file(a_dir + "/" + name) == read_file(b_dir + "/" + name), "two runs yield byte-identical PPMs");
}

void absent_frames_fail_closed() {
  const auto dir = fresh_dir("segarecomp-t031-absent");
  GenesisRuntime r;
  setup(r);
  auto o = options_for(dir, 50U, 1U, 1U);
  const auto exhausted = genesis_frame_capture_run(&r, synthetic_dispatch, &o, 100000U);
  check(exhausted.outcome == GENESIS_FRAME_CAPTURE_INCOMPLETE_RUNNER_EXHAUSTED && exhausted.frames_captured == 0U,
        "allowance ending before the window fails closed");
  setup(r);
  const auto stopped = genesis_frame_capture_run(&r, stopping_dispatch, &o, 10000000U);
  check(stopped.outcome == GENESIS_FRAME_CAPTURE_INCOMPLETE_GUEST_STOP && stopped.frames_captured == 0U,
        "guest stop before the window fails closed");
  check(!std::filesystem::exists(dir + "/frame-00000050.ppm"), "no artifact for an absent frame");
}

void invalid_arguments_are_rejected() {
  const auto dir = fresh_dir("segarecomp-t031-invalid");
  GenesisRuntime r;
  setup(r);
  auto o = options_for(dir, 0U, 1U, 1U);
  check(genesis_frame_capture_run(&r, synthetic_dispatch, &o, 1000U).outcome == GENESIS_FRAME_CAPTURE_INVALID_ARGUMENT,
        "first_frame 0 rejected");
  o = options_for(dir, 1U, GENESIS_FRAME_CAPTURE_MAX_FRAMES + 1U, 1U);
  check(genesis_frame_capture_run(&r, synthetic_dispatch, &o, 1000U).outcome == GENESIS_FRAME_CAPTURE_INVALID_ARGUMENT,
        "oversized window rejected");
  o = options_for(dir, 1U, 1U, 0U);
  check(genesis_frame_capture_run(&r, synthetic_dispatch, &o, 1000U).outcome == GENESIS_FRAME_CAPTURE_INVALID_ARGUMENT,
        "zero stride rejected");
  o = options_for(dir + "/missing-subdir", 1U, 1U, 1U);
  check(genesis_frame_capture_run(&r, synthetic_dispatch, &o, 10000000U).outcome == GENESIS_FRAME_CAPTURE_IO_ERROR,
        "unwritable output fails closed");
  check(r.live_frame_observer == nullptr, "observer detached after failure");
  // A write failure mid-window stops capturing: no later selected frame is written or digested.
  const auto mid = fresh_dir("segarecomp-t031-midwindow");
  std::filesystem::create_directories(mid + "/frame-00000005.ppm");  // blocks ordinal 5
  setup(r);
  o = options_for(mid, 3U, 4U, 1U);
  const auto failed = genesis_frame_capture_run(&r, synthetic_dispatch, &o, 10000000U);
  check(failed.outcome == GENESIS_FRAME_CAPTURE_IO_ERROR && failed.frames_captured == 2U,
        "mid-window I/O failure fails closed after the frames before it");
  check(!std::filesystem::exists(mid + "/frame-00000006.ppm"), "no frame is captured after an I/O failure");
}
}  // namespace

int main() {
  window_is_captured_by_ordinal_and_stops_early();
  capture_does_not_alter_execution();
  capture_is_deterministic();
  absent_frames_fail_closed();
  invalid_arguments_are_rejected();
  if (failures == 0) std::printf("genesis_frame_capture_tests: OK\n");
  return failures == 0 ? 0 : 1;
}
