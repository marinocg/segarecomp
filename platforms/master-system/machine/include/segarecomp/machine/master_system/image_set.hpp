#pragma once

// Generation-time ImageSet for a validated SMS cartridge (SEG-009-T002; ADR 0058 reference shape, machine contract 4.2).
//
//   sega      image 1 invariant: ROM $0000-$03FF at logical $0000 (one window); image 2+n banked: 16 KiB bank n,
//             admissible in slot 0 (base $0000, offsets $0400+), slot 1 (base $4000) and slot 2 (base $8000), so its
//             owners are window-relative. Nothing at $C000-$FFFF (RAM is never code).
//   rom_only  image 1 invariant: the whole 32 KiB ROM at logical $0000.
//
// Windows and identities are derived from the same table the run-time memory map uses (sms_mapper_contract.h).

#include "segarecomp/codegen/c11/z80.hpp"
#include "segarecomp/machine/master_system/cartridge.hpp"

namespace segarecomp::machine::master_system {

[[nodiscard]] codegen::z80::ImageSet build_image_set(const IngestResult& cartridge);

}  // namespace segarecomp::machine::master_system
