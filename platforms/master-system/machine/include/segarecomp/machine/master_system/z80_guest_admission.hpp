#pragma once

// Static admission of injected Master System guest code (SEG-147-T002, ADR 0324). The bounded assembler (libs/media) produces bytes and instruction boundaries; this unit lets the Z80 CPU OWNER decide
// whether those bytes may enter a cartridge image: every instruction must be decoded by the CPU library at exactly the assembled boundary, its control successors must be statically known, and execution
// must never leave the admitted instructions by falling through. It decodes at generation time only; nothing here exists in a generated program.

#include <optional>

#include "segarecomp/mod.hpp"

namespace segarecomp::machine::master_system {

// A `mod::GuestAdmitter`. Stable class: E_GUEST_NOT_ADMITTED.
[[nodiscard]] std::optional<mod::Rejection> admit_z80_guest(const mod::GuestListing &listing);

}  // namespace segarecomp::machine::master_system
