// ADR 0277: registry-level properties of the static-first Z80 producer's output: static provenance, activation signatures and ordinals as
// the existing registry defines them, the equality/equivalence relations, and the typed producer vocabulary. No ROM, no compiler.
#include <cstdio>
#include <string>

#include "segarecomp/machine/genesis/z80_images.hpp"
#include "segarecomp/machine/genesis/z80_static_producer.hpp"

namespace gz = segarecomp::machine::genesis::z80;

namespace {
int failures = 0;
void check(bool ok, const char* label) {
  std::printf("%-5s %s\n", ok ? "ok" : "FAIL", label);
  if (!ok) ++failures;
}

gz::Epoch epoch(std::uint8_t code, std::uint16_t length, std::uint8_t carry = 0) {
  gz::Epoch e;
  for (std::uint16_t i = 0; i < length; ++i) {
    e.ram[i] = static_cast<std::uint8_t>(code + i);
    e.written[i >> 3] = static_cast<std::uint8_t>(e.written[i >> 3] | (1U << (i & 7U)));
  }
  e.ram[gz::kRamBytes - 16] = carry;  // data outside the written extent
  return e;
}

bool has(const segarecomp::ImageProvenance& p, const std::string& key, const std::string& value) {
  for (const auto& fact : p.evidence)
    if (fact.name == key && fact.value == value) return true;
  return false;
}
}  // namespace

int main() {
  // ---- provenance: a static image never claims the observed evidence class --------------------------------------------------
  gz::Registry statics;
  std::uint32_t ordinal = 0;
  check(statics.add(epoch(10, 40), ordinal, gz::Derivation::static_reconstruction, "rom_copy") == gz::AddOutcome::added && ordinal == 1, "a static epoch is added as ordinal 1");
  check(statics.add(epoch(50, 90), ordinal, gz::Derivation::static_reconstruction, "kosinski") == gz::AddOutcome::added && ordinal == 2, "the next gets ordinal 2");
  check(statics.add(epoch(10, 40), ordinal, gz::Derivation::static_reconstruction, "rom_copy") == gz::AddOutcome::existing && ordinal == 1, "the same driver again re-binds ordinal 1");
  gz::Epoch restart;  // an empty hold window
  check(statics.add(restart, ordinal, gz::Derivation::static_reconstruction, "rom_stores") == gz::AddOutcome::restart && ordinal == 1, "an empty window is a restart of the bound image");
  check(statics.images().size() == 2, "two distinct images");

  const segarecomp::ExecutableImageSet set = gz::executable_images(statics);
  check(set.images.size() == 2 && set.images[0].provenance.authority == segarecomp::ImageAuthority::static_proof &&
            set.images[0].provenance.producer == "genesis.z80_static_producer",
        "static images are static_proof from the static producer");
  check(has(set.images[0].provenance, "derivation", "static_reconstruction") && has(set.images[0].provenance, "proof", "rom_copy") &&
            has(set.images[1].provenance, "proof", "kosinski"),
        "the derivation and the proof mechanism are evidence facts");
  check(!has(set.images[0].provenance, "derivation", "runnable_epoch_snapshot"), "no static image claims runnable_epoch_snapshot");
  check(set.images[0].verification == segarecomp::ImageVerification::structural, "verification stays the RAM-backed structural guard");
  check(segarecomp::validate_executable_image_set(set).ok(), "the projected set validates");

  gz::Registry observed;
  (void)observed.add(epoch(10, 40), ordinal);
  const segarecomp::ExecutableImageSet observed_set = gz::executable_images(observed);
  check(observed_set.images[0].provenance.authority == segarecomp::ImageAuthority::bounded_build_time_materialization &&
            has(observed_set.images[0].provenance, "derivation", "runnable_epoch_snapshot"),
        "an observed image keeps its original authority and evidence");

  // ---- mixed registry: static prefix, discovered tail ----------------------------------------------------------------------
  gz::Registry mixed;
  (void)mixed.add(epoch(10, 40), ordinal, gz::Derivation::static_reconstruction, "rom_copy");
  (void)mixed.add(epoch(50, 90), ordinal);  // discovered by the fixed point
  const segarecomp::ExecutableImageSet mixed_set = gz::executable_images(mixed);
  check(mixed_set.images[0].provenance.authority == segarecomp::ImageAuthority::static_proof &&
            mixed_set.images[1].provenance.authority == segarecomp::ImageAuthority::bounded_build_time_materialization,
        "per-image authority in a mixed registry");

  // ---- the relations ---------------------------------------------------------------------------------------------------------
  gz::Registry same_a, same_b;
  (void)same_a.add(epoch(10, 40), ordinal, gz::Derivation::static_reconstruction, "rom_copy");
  (void)same_b.add(epoch(10, 40), ordinal);
  check(gz::registries_equal(same_a, same_b) && gz::compare_registries(same_a, same_b) == gz::RegistryRelation::identical,
        "provenance does not take part in equality: the same epochs are identical");
  gz::Registry carry;
  (void)carry.add(epoch(10, 40, 0x77), ordinal);
  check(!gz::registries_equal(same_a, carry), "carry-over data outside the extents breaks strict equality (never silently relaxed)");
  check(gz::compare_registries(same_a, carry) == gz::RegistryRelation::equivalent_modulo_carry_over, "...and is reported as equivalence modulo carry-over, explicitly");
  gz::Registry other_code;
  (void)other_code.add(epoch(11, 40), ordinal);
  check(gz::compare_registries(same_a, other_code) == gz::RegistryRelation::different, "different code is a contradiction");
  gz::Registry swapped_a, swapped_b;
  (void)swapped_a.add(epoch(10, 40), ordinal);
  (void)swapped_a.add(epoch(50, 90), ordinal);
  (void)swapped_b.add(epoch(50, 90), ordinal);
  (void)swapped_b.add(epoch(10, 40), ordinal);
  check(gz::compare_registries(swapped_a, swapped_b) == gz::RegistryRelation::different, "a different ordinal order is a contradiction");
  check(gz::compare_registries(swapped_a, same_a) == gz::RegistryRelation::different, "a different image count is a contradiction");

  // ---- the vocabulary ---------------------------------------------------------------------------------------------------------
  check(std::string(gz::static_outcome_name(gz::StaticOutcome::complete)) == "complete" && std::string(gz::static_outcome_name(gz::StaticOutcome::resource_bound)) == "resource_bound",
        "outcome names");
  check(std::string(gz::static_refusal_name(gz::StaticRefusal::decompression_unproven)) == "decompression_unproven", "decompression_unproven is a typed refusal");
  check(!gz::static_refusal_affects_epochs(gz::StaticRefusal::step_bound) && gz::static_refusal_affects_epochs(gz::StaticRefusal::length_unknown),
        "resource refusals are not epoch refusals");
  check(gz::kosinski_routine_form_count() == 2, "two recognised Kosinski routine forms (reference, library)");

  // ---- an empty/garbage image: typed, bounded, no crash -----------------------------------------------------------------------
  const std::vector<std::uint8_t> tiny(8, 0);
  const gz::StaticProduction none = gz::produce_static_epochs(tiny);
  check(none.epochs.empty() && none.outcome != gz::StaticOutcome::complete, "an image with no Z80 upload proves nothing");
  gz::StaticProduction production;
  gz::Registry preload_target;
  check(gz::preload_registry(preload_target, production) == 0 && preload_target.images().empty(), "preloading nothing registers nothing");

  return failures == 0 ? 0 : 1;
}
