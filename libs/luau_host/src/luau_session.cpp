// SEG-137-T006/T007 (ADR 0295): the deterministic fixture session. See luau_api.hpp.
#include "segarecomp/luau_api.hpp"

#include "segarecomp/sha256.hpp"

#include <algorithm>
#include <array>

namespace segarecomp::luau {

namespace {

constexpr std::size_t kLogKept = 1024;

// A synthetic, ROM-free picture: a diagonal pattern that moves with the frame number and is seeded by the first three bytes of the image digest.
void base_frame(std::vector<std::uint8_t> &rgb, std::uint64_t frame, const std::array<std::uint8_t, 3> &seed) {
  rgb.resize(static_cast<std::size_t>(kFixtureWidth) * kFixtureHeight * 3U);
  const unsigned f = static_cast<unsigned>(frame & 0xFFFFU);
  std::size_t i = 0;
  for (unsigned y = 0; y < static_cast<unsigned>(kFixtureHeight); ++y)
    for (unsigned x = 0; x < static_cast<unsigned>(kFixtureWidth); ++x) {
      rgb[i++] = static_cast<std::uint8_t>((x * 3U + f + seed[0]) & 255U);
      rgb[i++] = static_cast<std::uint8_t>((y * 5U + 2U * f + seed[1]) & 255U);
      rgb[i++] = static_cast<std::uint8_t>(((x ^ y) + 7U * f + seed[2]) & 255U);
    }
}

void composite(std::vector<std::uint8_t> &rgb, const OverlayImage &overlay) {
  for (int y = 0; y < std::min(overlay.height, kFixtureHeight); ++y)
    for (int x = 0; x < std::min(overlay.width, kFixtureWidth); ++x) {
      const std::uint8_t *s = &overlay.rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(overlay.width) + static_cast<std::size_t>(x)) * 4U];
      if (s[3] == 0) continue;   // fully transparent pixels leave the picture alone
      std::uint8_t *d = &rgb[(static_cast<std::size_t>(y) * kFixtureWidth + static_cast<std::size_t>(x)) * 3U];
      for (int c = 0; c < 3; ++c) d[c] = static_cast<std::uint8_t>((s[c] * s[3] + d[c] * (255U - s[3]) + 127U) / 255U);
    }
}

std::uint8_t hex_byte(std::string_view h, std::size_t i) {
  const auto nib = [](char c) -> unsigned { return c >= '0' && c <= '9' ? static_cast<unsigned>(c - '0') : c >= 'a' && c <= 'f' ? static_cast<unsigned>(c - 'a' + 10) : 0U; };
  return i * 2 + 1 < h.size() ? static_cast<std::uint8_t>(nib(h[i * 2]) * 16 + nib(h[i * 2 + 1])) : 0;
}

}  // namespace

SessionResult run_fixture_session(const mod::Package &package, const CompiledMod &compiled, std::string_view image_sha256, std::uint64_t frames) {
  SessionResult r;
  if (frames == 0 || frames > kSessionFramesMax) {
    r.refusal = "the frame count is out of range";
    return r;
  }
  if (mod::trust_class(package.manifest) == "native") {
    r.refusal = "a fixture session runs data and Luau-only packages; a package with native code is not run";
    return r;
  }
  if (compiled.empty() || compiled.package_identity() != package.identity) {
    r.refusal = "the compiled scripts do not belong to this package";
    return r;
  }
  std::unique_ptr<Vm> vm = Vm::create(package.identity, limits_for(package.manifest), host_config_for(package.manifest));
  if (!vm) {
    r.refusal = "the VM cannot be created";
    return r;
  }
  const std::array<std::uint8_t, 3> seed = {hex_byte(image_sha256, 0), hex_byte(image_sha256, 1), hex_byte(image_sha256, 2)};
  const auto fault = [&](const CallResult &c, const char *callback, std::uint64_t frame) {
    if (c.ok || r.fault != Fault::none) return;
    r.fault = c.fault;
    r.fault_frame = frame;
    r.diagnostic = std::string("E_MOD_FAULT cause=") + fault_name(c.fault) + " mod=" + package.manifest.id + " callback=" + callback + " frame=" + std::to_string(frame) + ": " + c.message;
  };
  fault(vm->load(compiled), "load", 0);
  Output out;
  std::string chain;
  std::vector<std::uint8_t> rgb;
  for (std::uint64_t frame = 0; frame < frames; ++frame) {
    OverlayImage overlay;
    bool have_overlay = false;
    if (!vm->disabled()) {
      FrameInput in;
      in.number = frame;
      in.values = {{"score", static_cast<std::int64_t>(frame * 10U)}, {"lives", 3}, {"timer", static_cast<std::int64_t>(frames - frame)}};
      if (frame == 0) {
        fault(vm->run_callback("reset", in, 0, out), "reset", 0);
        for (std::string &l : out.log)
          if (r.log.size() < kLogKept) r.log.push_back(std::move(l));
      }
      if (!vm->disabled()) {
        fault(vm->run_callback("render", in, 1, out), "render", frame);
        if (!vm->disabled()) {   // a faulted callback produced nothing
          for (std::string &l : out.log)
            if (r.log.size() < kLogKept) r.log.push_back(std::move(l));
          for (Note &n : out.notes)
            if (r.notes.size() < kNotesMax) r.notes.push_back(std::move(n));
          if (!out.overlay.empty()) {
            overlay = rasterize_overlay(out.overlay);
            have_overlay = true;
            ++r.overlay_frames;
          }
        }
      }
    }
    base_frame(rgb, frame, seed);
    if (have_overlay) composite(rgb, overlay);
    const std::string digest = sha256_hex(rgb);
    r.frame_sha256.push_back(digest);
    chain += digest + "\n";
  }
  r.session_sha256 = sha256_hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(chain.data()), chain.size()));
  r.last_frame = std::move(rgb);
  r.frames = frames;
  r.nondeterministic = vm->nondeterministic();
  r.leaked_bytes = vm->close();
  r.ok = true;
  return r;
}

}  // namespace segarecomp::luau
