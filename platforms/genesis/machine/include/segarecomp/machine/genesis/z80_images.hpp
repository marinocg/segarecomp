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
#include <string_view>
#include <vector>

#include "segarecomp/codegen/c11/z80.hpp"
#include "segarecomp/recompiler/executable_image.hpp"

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

// How an image entered the registry (provenance only; it never takes part in the content hash, the signature or the equality proof).
enum class Derivation : std::uint8_t {
  runnable_epoch_snapshot,  // observed: the 8 KiB RAM snapshot of a runnable transition of a bounded build-time run (ADR 0073)
  static_reconstruction,    // reconstructed before any Z80 compilation from immutable cartridge bytes (ADR 0277, authority static_proof)
};

struct Image {
  std::uint32_t ordinal = 0;  // 1-based, order of first activation; also the generated code-image identity
  Digest content{};
  Digest signature{};
  std::array<std::uint8_t, kRamBytes> ram{};  // the snapshot the image was compiled from
  Derivation derivation = Derivation::runnable_epoch_snapshot;
  std::string proof;  // static_reconstruction only: the name of the strongest proof mechanism (static_proof_name of z80_static_producer.hpp)
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
  // `derivation`/`proof` describe a newly registered image only; a signature already registered keeps the derivation it first had.
  AddOutcome add(const Epoch& epoch, std::uint32_t& bound_ordinal, Derivation derivation = Derivation::runnable_epoch_snapshot,
                 std::string_view proof = {});
  [[nodiscard]] const std::vector<Image>& images() const noexcept { return images_; }
  [[nodiscard]] std::uint32_t last_bound() const noexcept { return last_bound_; }

 private:
  std::vector<Image> images_;
  std::uint32_t last_bound_ = 0;
};

// SEG-028-T003 (ADR 0077): the producer boundary. A registry is described as CPU-neutral executable images: one image per registered
// snapshot, id = ordinal, the 16 KiB Z80 code window (the 8 KiB snapshot twice) as owned bytes with one mapping at $0000, verification
// `structural` (the RAM-backed guard), and the activation signature as the platform selector. The authority and producer name belong to
// whoever filled the registry: the build-time materializer by default; a future static producer (or a test stand-in) that fills an equal
// registry declares `static_proof` and yields the same projected ImageSet, so registry, AOT, runtime selection and devices are unchanged
// (ADR 0073 decision 8).
struct ImageProducer {
  ImageAuthority authority = ImageAuthority::bounded_build_time_materialization;
  std::string name = "genesis.z80_materializer";
};
//
// SEG-115 (ADR 0259): `structural_alternatives` additionally attaches to every image the finite alternative byte values that the static mutation analysis
// (libs/cpu/z80 mutation.hpp) proves some store of the image can write over its own code (both RAM mirrors), so the emitter compiles the forms those
// bytes allow. It never claims completeness; false (the default, and the Genesis build) leaves the images exactly as before.
struct AlternativesSummary {
  std::size_t images_with = 0;      // images carrying alternatives
  std::size_t bytes = 0;            // alternative byte values over all images (mirrors counted once)
  std::size_t images_over_bound = 0;  // images whose candidate bytes exceeded the pre-registered bound (they carry none)
  Digest identity{};                // SHA-256 over every image's (ordinal, offset, value) list; zero when unused
};
[[nodiscard]] ExecutableImageSet executable_images(const Registry& registry, const ImageProducer& producer = {},
                                                   bool structural_alternatives = false, AlternativesSummary* summary = nullptr);

// Registry equality (the replacement proof): the same ordered images with the same ordinal, content hash, signature and snapshot.
[[nodiscard]] bool registries_equal(const Registry& left, const Registry& right);

// ADR 0277: the relation between a statically produced registry and an observed one. The Z80 may write its own RAM between epochs and the
// 68K-written extents of an activation signature cover only the bytes the 68K stored, so a static snapshot and an observed snapshot can
// legitimately differ in bytes OUTSIDE every image's written extents ("carry-over"). Nothing else may differ.
enum class RegistryRelation : std::uint8_t {
  identical,                        // registries_equal
  equivalent_modulo_carry_over,     // same count, ordinals, activation signatures; snapshots equal inside the extents (guaranteed by the signature) and differ only outside them
  different,                        // a different number, order, ordinal or signature: a contradiction
};
[[nodiscard]] RegistryRelation compare_registries(const Registry& left, const Registry& right);
[[nodiscard]] const char* registry_relation_name(RegistryRelation relation) noexcept;

// One RAM-backed banked image per registered snapshot: window $0000-$3FFF (the snapshot twice), identity = ordinal.
// = executable_images(registry) projected onto the Z80 emitter input with every image banked.
[[nodiscard]] codegen::z80::ImageSet build_image_set(const Registry& registry);

struct EmitRequest {
  std::filesystem::path directory;
  std::string stem = "genesis_z80";
  bool structural_alternatives = false;  // SEG-115: see executable_images; the Sega CD build only
  codegen::z80::EmitOptions codegen;  // `directory` and `stem` above override the nested values
};

struct EmitOutcome {
  std::string error;  // non-empty on any failure
  codegen::z80::EmitStats stats;
  AlternativesSummary alternatives;
  [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

[[nodiscard]] EmitOutcome emit_registry(const Registry& registry, const EmitRequest& request);

}  // namespace segarecomp::machine::genesis::z80
