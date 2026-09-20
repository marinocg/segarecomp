#pragma once

#include "segarecomp/rom.hpp"

#include <string>

namespace segarecomp {

// Emits the current pipeline manifest. Instruction-level C emission is a later milestone.
[[nodiscard]] std::string emit_c_manifest(const RomInfo &rom);

} // namespace segarecomp
