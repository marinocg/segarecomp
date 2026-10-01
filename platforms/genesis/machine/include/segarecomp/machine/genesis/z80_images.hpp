#pragma once

// Genesis materialized Z80 images (SEG-032-T003; ADR 0073, contract sections 5-7).
//
// An *image epoch* is the 8,192-byte sound-RAM snapshot at the transition to a Z80-runnable state after a reset/upload
// epoch, together with the bitmap of the RAM bytes the 68K wrote during the hold window. This module owns the two keys
// derived from it, the deterministic registry of the distinct images and their emission through the existing broad Z80 AOT:
//   content hash  names a compiled image (the full snapshot); a build artifact, never runtime selection authority
//   signature     the runtime selection key S1*: the 68K-written extents of the hold window (contract section 7)
// An epoch whose hold window is empty (a plain restart) re-binds the previously bound image and creates none.
//
// Files written by emit_registry (deterministic, byte-identical across runs, nothing left behind on failure):
//   <stem>*.c / <stem>.h ...  the sharded image C from codegen::z80::emit_image_set (every image RAM-backed), in <stem>.units
//   <stem>_registry.c         the ordered signature / content-hash tables and genesis_z80_image_for_signature(), appended to
//                             <stem>.units (declared by platforms/genesis/runtime/z80_registry.h)

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "segarecomp/codegen/c11/z80.hpp"

namespace segarecomp::machine::genesis::z80 {

inline constexpr std::size_t kRamBytes = 8192;
inline constexpr std::size_t kWindowBytes = 0x4000;  // two mirrors of the sound RAM: the Z80 code window $0000-$3FFF
inline constexpr std::size_t kBitmapBytes = kRamBytes / 8;
// The materialization pass's finite image bound (ADR 0073, frozen by SEG-032-T008: the authorized workloads show at most 2 images;
// the bound is 4x that, and the ceiling kMaxImagesCeiling of z80_materialization.hpp is 16).
inline constexpr std::size_t kMaxImages = 8;

using Digest = std::array<std::uint8_t, 32>;

struct Epoch {
  std::array<std::uint8_t, kRamBytes> ram{};
  std::array<std::uint8_t, kBitmapBytes> written{};  // bit i of byte i/8: Z80 RAM byte i written by the 68K in the hold window
};

struct Extent {
  std::uint16_t offset = 0;
  std::uint16_t length = 0;
};

[[nodiscard]] std::vector<Extent> extents(std::span<const std::uint8_t, kBitmapBytes> written);
[[nodiscard]] Digest content_digest(std::span<const std::uint8_t, kRamBytes> ram);
// Precondition: at least one written byte (the caller handles the empty-window restart).
[[nodiscard]] Digest signature_digest(const Epoch& epoch);
[[nodiscard]] std::string to_hex(const Digest& digest);

struct Image {
  std::uint32_t ordinal = 0;  // 1-based, order of first activation; also the generated code-image identity
  Digest content{};
  Digest signature{};
  std::array<std::uint8_t, kRamBytes> ram{};  // the snapshot the image was compiled from
};

enum class AddOutcome : std::uint8_t {
  added,           // a new image was registered
  existing,        // the signature is already registered (the snapshot may differ in carry-over data): the same image
  restart,         // empty hold window: the previously bound image is re-bound, nothing registered
  bound_exceeded,  // the image bound is reached: a build failure (ADR 0073)
};

class Registry {
 public:
  // Registers the epoch's image. `bound_ordinal` receives the ordinal this epoch binds (0 when none: a restart before any epoch).
  AddOutcome add(const Epoch& epoch, std::uint32_t& bound_ordinal);
  [[nodiscard]] const std::vector<Image>& images() const noexcept { return images_; }
  [[nodiscard]] std::uint32_t last_bound() const noexcept { return last_bound_; }

 private:
  std::vector<Image> images_;
  std::uint32_t last_bound_ = 0;
};

// One RAM-backed banked image per registered snapshot: window $0000-$3FFF (the snapshot twice), identity = ordinal.
[[nodiscard]] codegen::z80::ImageSet build_image_set(const Registry& registry);

struct EmitRequest {
  std::filesystem::path directory;
  std::string stem = "genesis_z80";
  codegen::z80::EmitOptions codegen;  // `directory` and `stem` above override the nested values
};

struct EmitOutcome {
  std::string error;  // non-empty on any failure
  codegen::z80::EmitStats stats;
  [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

[[nodiscard]] EmitOutcome emit_registry(const Registry& registry, const EmitRequest& request);

}  // namespace segarecomp::machine::genesis::z80
