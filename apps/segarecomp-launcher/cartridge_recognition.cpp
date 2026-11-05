#include "cartridge_recognition.hpp"

#include <cctype>
#include <string_view>

// The narrow idea follows Genesis Plus GX (core/cart_hw/md_cart.c), which recognizes this board from the header's title instead of the image length. Stricter here, because the
// result selects a build declaration: the base and the attached cartridge must each be internally consistent (identity, declared ROM range, own header checksum, exact image size).
// The identities are the public catalog facts printed in every copy's header (title and product code); no image content is embedded and there is no digest list, so a region or
// revision that does not match stays `ambiguous` and is one explicit choice away.
namespace launcher {
namespace {

constexpr std::size_t slot_bytes = 0x200000;  // one cartridge of the board: 2 MiB (genesis_lock_on_slot_bytes in the product)
constexpr std::size_t system_at = 0x100, domestic_at = 0x120, overseas_at = 0x150, title_len = 48, serial_at = 0x180, serial_len = 14, checksum_at = 0x18E, range_at = 0x1A0;

constexpr std::string_view base_title = "SONIC & KNUCKLES", base_code = "MK-1563", attached_title = "SONIC THE HEDGEHOG 3", attached_code = "MK-1079";

std::string field(std::span<const std::uint8_t> header, std::size_t at, std::size_t length) {
  return std::string(reinterpret_cast<const char *>(header.data()) + at, length);
}

// Upper case with runs of spaces collapsed: both titles pad with spaces, and one pads between words.
std::string normalized(std::string text) {
  std::string out;
  for (const char c : text) {
    const auto u = static_cast<unsigned char>(c);
    if (std::isspace(u) != 0 || u == 0) { if (!out.empty() && out.back() != ' ') out.push_back(' '); }
    else out.push_back(static_cast<char>(std::toupper(u)));
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::uint32_t be32(std::span<const std::uint8_t> header, std::size_t at) {
  return (std::uint32_t{header[at]} << 24U) | (std::uint32_t{header[at + 1]} << 16U) | (std::uint32_t{header[at + 2]} << 8U) | std::uint32_t{header[at + 3]};
}

bool sega_signature(std::span<const std::uint8_t> header) { return field(header, system_at, 4) == "SEGA"; }

// The cartridge's identity: both title fields name it and the product code agrees.
bool identified(std::span<const std::uint8_t> header, std::string_view title, std::string_view code) {
  return normalized(field(header, domestic_at, title_len)) == title && normalized(field(header, overseas_at, title_len)) == title &&
         field(header, serial_at, serial_len).find(code) != std::string::npos && field(header, serial_at, 2) == "GM";
}

bool declares_one_slot(std::span<const std::uint8_t> header) { return be32(header, range_at) == 0 && be32(header, range_at + 4) == slot_bytes - 1U; }

// The header's own checksum over exactly this one cartridge's bytes.
bool checksum_valid(std::span<const std::uint8_t> slot) {
  std::uint16_t sum = 0;
  for (std::size_t i = cartridge_header_bytes; i + 1 < slot.size(); i += 2) sum = static_cast<std::uint16_t>(sum + ((slot[i] << 8U) | slot[i + 1]));
  return sum == static_cast<std::uint16_t>((slot[checksum_at] << 8U) | slot[checksum_at + 1]);
}

CartridgeFinding ambiguous(std::string note) { return CartridgeFinding{CartridgeKind::ambiguous, std::move(note)}; }

} // namespace

bool cartridge_header_resembles_lock_on(std::span<const std::uint8_t> image) {
  if (image.size() < cartridge_header_bytes || !sega_signature(image)) return false;
  const std::string domestic = normalized(field(image, domestic_at, title_len)), overseas = normalized(field(image, overseas_at, title_len));
  const auto names_both = [](const std::string &title) { return title.find("SONIC") != std::string::npos && title.find("KNUCKLES") != std::string::npos; };
  return names_both(domestic) || names_both(overseas) || field(image, serial_at, serial_len).find(base_code) != std::string::npos;
}

CartridgeFinding recognize_genesis_cartridge(std::span<const std::uint8_t> image) {
  if (!cartridge_header_resembles_lock_on(image)) return {};  // every ordinary cartridge, at any size, whatever follows its first 2 MiB
  if (image.size() != slot_bytes && image.size() != 2U * slot_bytes)
    return ambiguous("It looks like a lock-on cartridge, but its size is not the supported 2 MiB or 4 MiB.");
  const auto base = image.first(slot_bytes);
  if (!identified(base, base_title, base_code) || !declares_one_slot(base))
    return ambiguous("It looks like a lock-on cartridge, but its header does not identify a supported one.");
  if (!checksum_valid(base)) return ambiguous("It looks like a lock-on cartridge, but it seems to be modified (its header checksum does not match).");
  if (image.size() == slot_bytes) return {CartridgeKind::lock_on, {}};
  const auto attached = image.subspan(slot_bytes);
  if (!sega_signature(attached) || !declares_one_slot(attached))
    return ambiguous("It starts with a lock-on cartridge, but its second half is not a plausible attached cartridge.");
  if (!identified(attached, attached_title, attached_code))
    return ambiguous("It starts with a lock-on cartridge, but the attached cartridge is not one this launcher recognizes.");
  if (!checksum_valid(attached)) return ambiguous("It starts with a lock-on cartridge, but the attached cartridge seems to be modified (its header checksum does not match).");
  return {CartridgeKind::lock_on, {}};
}

} // namespace launcher
