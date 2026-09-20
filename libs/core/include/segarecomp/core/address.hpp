#pragma once

// SEG-014-T002: genuinely cross-target contracts (docs/architecture/
// post-seg007-architecture-refactor-contract.md, section 8.1 "core/"). This
// header owns only address-shaped facts that are already reused identically
// by every later stage (CPU, device, machine, runtime) and carry zero
// MC68000/Genesis/startup-scenario/controller-I/O/generated-C content.
//
// Provenance: relocated from include/segarecomp/moveq.hpp (SEG-002's slice
// header, which happened to be the only file with no MC68000/Genesis-
// specific content beyond the single `m68k_program` address-space value) per
// docs/architecture/seg-014-t001-symbol-migration-map.md's `core/` section.
//
// `MoveqImageOffset`'s eventual rename to a plain `ImageOffset` (the map's
// stated end state, converging toward the single type include/segarecomp/
// rom.hpp's independently-defined `ImageOffset` should also reuse) is
// deliberately NOT completed in this task: rom.hpp already declares its own
// `struct ImageOffset` in the same `segarecomp` namespace, and rom.hpp is
// explicitly out of scope for T002 (its physical move is T004's job, per the
// migration map's own "Ownership decisions" section). Renaming this type to
// `ImageOffset` here would collide with rom.hpp's own definition in every
// translation unit that includes both headers (e.g. apps/segarecomp/main.cpp, several
// test harnesses) without T002 touching rom.hpp at all -- exactly the
// "leave rom.hpp/rom.cpp alone, out of scope" boundary this task must
// respect. T002 therefore relocates the type unchanged in name (only its
// physical location moves, from moveq.hpp to here) so both types can safely
// coexist until T004 physically moves rom.hpp's reset-image validator and
// makes it reuse this one, at which point the rename to `ImageOffset`
// becomes possible without a collision.
//
// `MappingClaim` is a bounded, additive placement correction discovered while
// making this boundary physically real: the migration map lists it under
// `machine/genesis/` (grouped there because the Genesis-specific frontend
// composition is its only current user), but the type's own fields
// (`name`, a target M68kProgramAddress range, and a MoveqImageOffset range)
// are already fully generic -- exactly the "target address types / image
// offsets" shape section 8.1 asks core/ to own, and the exact same argument
// the map already applies to MoveqImageOffset itself. It is placed here
// (rather than left duplicated or forward-declared) because the temporary
// SEG-002/SEG-003 direct-flow compatibility surface (cpu/m68k/direct_flow.hpp)
// already depends on it structurally; leaving it in the Genesis-shaped
// m68k_pipeline.hpp would force cpu/m68k to depend on machine/genesis once
// that header's remaining Genesis content physically relocates there, which
// is exactly the forbidden dependency direction this task's own build
// boundary must reject. No field, name, or behavior changes as part of this
// relocation.

#include <cstdint>
#include <string>

namespace segarecomp {

enum class TargetAddressSpace { m68k_program };

struct M68kProgramAddress {
  TargetAddressSpace space{TargetAddressSpace::m68k_program};
  std::uint32_t value{};
};

// Relocated from moveq.hpp; identical name and shape (see this header's own
// doc comment above for why the map's eventual `ImageOffset` rename is
// deferred to SEG-014-T004).
struct MoveqImageOffset {
  std::uint64_t value{};
};

struct ByteLength {
  std::uint32_t value{};
};

// Currently one-value (mc68000); grows to z80/sh2 later without becoming a
// new type, exactly like TargetAddressSpace above.
enum class CpuVariant { mc68000 };

// A generic "this program-address range came from this image-offset range"
// fact. See this header's own doc comment above for why it lives here
// instead of the migration map's original machine/genesis/ classification.
struct MappingClaim {
  std::string name;
  M68kProgramAddress target_begin{};
  M68kProgramAddress target_end{};
  MoveqImageOffset image_begin{};
  MoveqImageOffset image_end{};
};

} // namespace segarecomp
