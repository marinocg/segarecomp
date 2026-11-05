// SEG-122-T009 (ADR 0268): the launcher's recognition of the one uncommon Genesis cartridge board it builds automatically. A deliberately small boundary: it
// reads an image's own header fields and picks the launcher's DEFAULT for the explicit `segarecomp build --mapper lock_on` declaration. The build tool itself
// never guesses, and nothing game-specific exists below this file: the hardware support is the generic lock-on board of ADR 0268.
//
// Never consulted: the file name, the library title, a ZIP name, a generic SEGA header alone, a second apparent header without a verified base, or a patch.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace launcher {

enum class CartridgeKind {
  standard,   // the ordinary Genesis cartridge: no declaration, the historical build
  lock_on,    // a recognized lock-on image: `--mapper lock_on`
  ambiguous,  // it resembles a lock-on image but cannot be identified: behaves as standard, the user is told why and may override
};

struct CartridgeFinding {
  CartridgeKind kind = CartridgeKind::standard;
  std::string note;  // ambiguous only: the plain-language reason shown to the user
  [[nodiscard]] std::string mapper() const { return kind == CartridgeKind::lock_on ? std::string("lock_on") : std::string(); }
};

// The header bytes a caller must supply to `cartridge_header_resembles_lock_on`.
inline constexpr std::size_t cartridge_header_bytes = 0x200;

// Does this header claim to be the lock-on base at all? Needs only the first `cartridge_header_bytes` bytes, so a library check can skip reading ordinary games.
[[nodiscard]] bool cartridge_header_resembles_lock_on(std::span<const std::uint8_t> image);

// Deterministic and total on any input. See the rules in cartridge_recognition.cpp.
[[nodiscard]] CartridgeFinding recognize_genesis_cartridge(std::span<const std::uint8_t> image);

} // namespace launcher
