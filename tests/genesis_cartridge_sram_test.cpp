// ADR 0098: the header-declared cartridge SRAM descriptor and the shared access classifier.  Project-authored synthetic
// headers only; no commercial bytes.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "segarecomp/machine/genesis/cartridge_sram.hpp"

using namespace segarecomp;

namespace {

int failures = 0;

void check(bool ok, const char *label) {
  std::printf("%-5s %s\n", ok ? "ok" : "FAIL", label);
  if (!ok) ++failures;
}

std::vector<std::uint8_t> image(std::size_t size, bool ra, std::uint8_t type, std::uint8_t kind, std::uint32_t start,
                                std::uint32_t end) {
  std::vector<std::uint8_t> rom(size, 0U);
  if (!ra) return rom;
  rom[0x1B0] = 'R';
  rom[0x1B1] = 'A';
  rom[0x1B2] = type;
  rom[0x1B3] = kind;
  for (std::size_t i = 0; i < 4; ++i) {
    rom[0x1B4 + i] = static_cast<std::uint8_t>(start >> (24U - 8U * i));
    rom[0x1B8 + i] = static_cast<std::uint8_t>(end >> (24U - 8U * i));
  }
  return rom;
}

GenesisCartridgeSramHeaderStatus status_of(const std::vector<std::uint8_t> &rom) {
  return parse_genesis_cartridge_sram_header(rom).status;
}

}  // namespace

int main() {
  using Status = GenesisCartridgeSramHeaderStatus;

  // No descriptor -> no device.
  check(status_of(image(0x400, false, 0, 0, 0, 0)) == Status::absent, "no RA marker is absent");
  check(status_of(std::vector<std::uint8_t>(0x100, 0U)) == Status::absent, "an image shorter than the header is absent");

  // The standard battery-backed odd-byte SRAM: exact descriptor.
  {
    const auto parsed = parse_genesis_cartridge_sram_header(image(0x1000, true, 0xF8, 0x20, 0x200001, 0x203FFF));
    check(parsed.status == Status::declared && parsed.descriptor.has_value(), "F8 20 odd SRAM is declared");
    const auto &d = *parsed.descriptor;
    check(d.start == 0x200001U && d.end == 0x203FFFU && d.odd_lane && d.battery_backed && d.supported && d.always_mapped,
          "descriptor fields (odd lane, battery, supported, ROM below start -> always mapped)");
    check(d.storage_bytes() == 0x2000U, "dense storage is one byte per lane address");
  }
  {
    const auto parsed = parse_genesis_cartridge_sram_header(image(0x200400, true, 0xB8, 0x20, 0x200001, 0x2003FF));
    check(parsed.descriptor && !parsed.descriptor->battery_backed && !parsed.descriptor->always_mapped &&
              parsed.descriptor->storage_bytes() == 0x200U,
          "B8 (no battery) over a ROM that extends into the extent is overlaid, not always mapped");
  }
  {
    const auto parsed = parse_genesis_cartridge_sram_header(image(0x1000, true, 0xF0, 0x20, 0x200000, 0x2003FE));
    check(parsed.descriptor && !parsed.descriptor->odd_lane && parsed.descriptor->supported &&
              parsed.descriptor->storage_bytes() == 0x200U,
          "F0 even-lane SRAM is supported");
  }
  // 16-bit SRAM: recorded but not supported.
  {
    const auto parsed = parse_genesis_cartridge_sram_header(image(0x1000, true, 0xE0, 0x20, 0x200000, 0x203FFF));
    check(parsed.status == Status::declared && parsed.descriptor && !parsed.descriptor->supported,
          "E0 16-bit SRAM is declared but unsupported");
  }
  // EEPROM and other extra memory is not SRAM.
  check(status_of(image(0x1000, true, 0xE8, 0x40, 0x200001, 0x200001)) == Status::unsupported_type, "E8 40 EEPROM is unsupported");
  check(status_of(image(0x1000, true, 0xF8, 0x21, 0x200001, 0x203FFF)) == Status::malformed, "unknown second byte is malformed");
  // Malformed descriptors.
  check(status_of(image(0x1000, true, 0xF8, 0x20, 0x203FFF, 0x200001)) == Status::malformed, "start > end");
  check(status_of(image(0x1000, true, 0xF8, 0x20, 0x1FFFFF, 0x203FFF)) == Status::malformed, "start below the cartridge window");
  check(status_of(image(0x1000, true, 0xF8, 0x20, 0x3F0001, 0x400001)) == Status::malformed, "end beyond the cartridge window");
  check(status_of(image(0x1000, true, 0xF8, 0x20, 0x200001, 0x210001)) == Status::malformed, "span above the bounded capacity");
  check(status_of(image(0x1000, true, 0xF8, 0x20, 0x200000, 0x203FFF)) == Status::malformed, "odd lane with an even start");
  check(status_of(image(0x1000, true, 0xF0, 0x20, 0x200001, 0x203FFF)) == Status::malformed, "even lane with an odd start");
  check(status_of(image(0x1000, true, 0xF8, 0x20, 0x200001, 0x203FFE)) == Status::malformed, "odd lane with an even end");
  check(status_of(image(0x1000, true, 0xC8, 0x20, 0x200001, 0x203FFF)) == Status::malformed, "undocumented type byte");
  check(status_of(image(0x1000, true, 0xE0, 0x20, 0x200001, 0x203FFF)) == Status::malformed, "16-bit SRAM must start even and end odd");

  // Access classification on the odd lane 0x200001..0x20000F.
  const auto classify = [](std::uint32_t address, std::uint32_t width) {
    return segarecomp_genesis_cartridge_sram_classify(0x200001U, 0x20000FU, 1U, address, width);
  };
  check(classify(0x200001U, 1U) == SEGARECOMP_GENESIS_CARTRIDGE_SRAM_ACCESS_BYTE, "first lane byte");
  check(classify(0x20000FU, 1U) == SEGARECOMP_GENESIS_CARTRIDGE_SRAM_ACCESS_BYTE, "last lane byte");
  check(classify(0x200000U, 1U) == SEGARECOMP_GENESIS_CARTRIDGE_SRAM_ACCESS_OUTSIDE, "byte just below the extent is ROM");
  check(classify(0x200010U, 1U) == SEGARECOMP_GENESIS_CARTRIDGE_SRAM_ACCESS_OUTSIDE, "byte just above the extent is ROM");
  check(classify(0x200002U, 1U) == SEGARECOMP_GENESIS_CARTRIDGE_SRAM_ACCESS_REJECTED, "opposite lane inside the extent is rejected");
  check(classify(0x200000U, 2U) == SEGARECOMP_GENESIS_CARTRIDGE_SRAM_ACCESS_REJECTED, "a word touching the extent is rejected");
  check(classify(0x200002U, 4U) == SEGARECOMP_GENESIS_CARTRIDGE_SRAM_ACCESS_REJECTED, "a long inside the extent is rejected");
  check(classify(0x20000EU, 2U) == SEGARECOMP_GENESIS_CARTRIDGE_SRAM_ACCESS_REJECTED, "a word straddling the last lane byte is rejected");
  check(classify(0x1FFFFEU, 2U) == SEGARECOMP_GENESIS_CARTRIDGE_SRAM_ACCESS_OUTSIDE, "a word ending below the extent is ROM");
  check(classify(0x200010U, 2U) == SEGARECOMP_GENESIS_CARTRIDGE_SRAM_ACCESS_OUTSIDE, "a word starting above the extent is ROM");
  check(segarecomp_genesis_cartridge_sram_control_write_admitted(1U, 0U) && segarecomp_genesis_cartridge_sram_control_write_admitted(1U, 3U),
        "BYTE control values 0..3 are admitted");
  check(!segarecomp_genesis_cartridge_sram_control_write_admitted(1U, 4U) && !segarecomp_genesis_cartridge_sram_control_write_admitted(1U, 0x80U) &&
            !segarecomp_genesis_cartridge_sram_control_write_admitted(2U, 1U),
        "undefined control bits and WORD writes are rejected");
  const auto d = *parse_genesis_cartridge_sram_header(image(0x1000, true, 0xF8, 0x20, 0x200001, 0x20000F)).descriptor;
  check(genesis_cartridge_sram_touches(d, 0x200002U, 1U) && !genesis_cartridge_sram_access_supported(d, 0x200002U, 1U) &&
            genesis_cartridge_sram_access_supported(d, 0x200003U, 1U),
        "descriptor helpers agree with the shared classifier");
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
