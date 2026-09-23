// SEG-007-T199 / ADR-0009 owner-1 + owner-2: focused coverage for bounded
// edge-sensitive finite-`Dn` refinement plus the immediate-subtraction /
// word-left-shift / partial-register byte-load finite-value transfers that the
// same proven SMPS-style `pc_index8` command-dispatch chain requires.
//
// Positive: a selector byte loaded into a quick-immediate-zeroed data register,
// bounded to a closed interval by a `CMPI.B #imm,Dn` + unsigned-`Bcc` guard
// pair, normalized by `SUBI.B #imm,Dn`, scaled by `LSL.W #2,Dn`, and consumed
// by a brief PC-relative indexed `JMP` resolves to an exact ADR-0009 Tier-1
// target set with no new emitter and no candidate-recall change.
//
// Negative / inverse: a genuinely unknown selector (no proven upper bits) stays
// fail-closed for a byte compare; a word compare whose interval exceeds the
// 256-member finite-value cap stays fail-closed; a signed branch condition is
// never consumed.

#include "segarecomp/cpu/m68k/static_discovery.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace {

class FlatImageDiscoveryEnvironment final : public segarecomp::M68kStaticDiscoveryEnvironment {
 public:
  FlatImageDiscoveryEnvironment(std::vector<std::uint8_t> image, std::uint32_t base)
      : image_(std::move(image)), base_(base) {}

  segarecomp::M68kInstructionSourceResult instruction_source(segarecomp::M68kProgramAddress pc) override {
    if (pc.value < base_ || pc.value >= base_ + image_.size()) {
      segarecomp::M68kInstructionSourceIssue issue{};
      issue.kind = segarecomp::M68kInstructionSourceIssueKind::unmapped;
      issue.address = pc;
      return issue;
    }
    const auto local = static_cast<std::size_t>(pc.value - base_);
    const segarecomp::DecodeSource source{segarecomp::CpuVariant::mc68000, pc, {local}};
    return segarecomp::M68kInstructionSource{std::span<const std::uint8_t>(image_), source, {local}};
  }
  // SEG-021-T028: optional project-authored admission rejection of one
  // direct-call-role candidate address (empty for every pre-T028 test).
  std::optional<std::uint32_t> reject_call_target;
  std::optional<segarecomp::M68kMappingIssue> admit_target(
      segarecomp::M68kProgramAddress target, segarecomp::M68kDiscoveryTargetRole role) override {
    if (reject_call_target.has_value() && role == segarecomp::M68kDiscoveryTargetRole::direct_call &&
        target.value == *reject_call_target) {
      segarecomp::M68kMappingIssue issue{};
      return issue;
    }
    return std::nullopt;
  }
  std::optional<segarecomp::DirectFlowDiagnostic> classify_memory_access(
      const segarecomp::M68kCpuMemoryAccessRequest &) override {
    return std::nullopt;
  }
  bool is_completion_rts(const segarecomp::InstructionProvenance &) override { return false; }
  std::optional<segarecomp::M68kImmutableCartridgeBytes> read_immutable_cartridge_bytes(
      segarecomp::M68kProgramAddress, std::uint32_t) override {
    return std::nullopt;
  }

 private:
  std::vector<std::uint8_t> image_;
  std::uint32_t base_;
};

void push16(std::vector<std::uint8_t> &image, std::uint32_t value) {
  image.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
  image.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

// Shared prologue+dispatch tail for the SMPS-style shape. `load_word` selects a
// word memory load (selector stays genuinely unknown) instead of the
// quick-zero + byte load partial-register producer. `compare_word` selects a
// single word compare instead of the byte compare guard pair. `signed_branch`
// selects a signed condition. `upper_immediate` is the inclusive upper bound
// tested (byte-compare path) or the word `<=` bound (word-compare path).
std::vector<std::uint8_t> smps_dispatch_image(bool load_word, bool compare_word, bool signed_branch,
                                              std::uint32_t upper_immediate) {
  std::vector<std::uint8_t> image;
  if (load_word) {
    image.push_back(0x30U); image.push_back(0x38U); push16(image, 0x0002U);  // MOVE.W ($0002).W,D0
    image.push_back(0x4EU); image.push_back(0x71U);                          // NOP (pad to offset 0x06)
  } else {
    image.push_back(0x70U); image.push_back(0x00U);                          // MOVEQ #0,D0
    image.push_back(0x10U); image.push_back(0x38U); push16(image, 0x0002U);  // MOVE.B ($0002).W,D0
  }
  // image.size() == 0x06 here.
  if (compare_word) {
    image.push_back(0x0CU); image.push_back(0x40U); push16(image, upper_immediate);  // CMPI.W #imm,D0   @0x06
    // BLS.W dispatch (0x0A + 2 + disp). dispatch is at 0x10.
    const std::uint8_t cc = signed_branch ? 0x6FU /*BLE*/ : 0x63U /*BLS*/;
    image.push_back(cc); image.push_back(0x00U); push16(image, 0x10U - 0x0CU);        // @0x0A
    image.push_back(0x4EU); image.push_back(0x71U);                                    // NOP pad @0x0E -> 0x10
  } else {
    image.push_back(0x0CU); image.push_back(0x00U); push16(image, 0x0005U);  // CMPI.B #5,D0     @0x06
    image.push_back(0x65U); image.push_back(0x00U); push16(image, 0x2CU - 0x0CU);  // BCS.W skip  @0x0A
    image.push_back(0x0CU); image.push_back(0x00U); push16(image, upper_immediate);  // CMPI.B #hi,D0 @0x0E
    const std::uint8_t cc = signed_branch ? 0x6DU /*BLT*/ : 0x62U /*BHI*/;
    image.push_back(cc); image.push_back(0x00U); push16(image, 0x2CU - 0x14U);  // Bcc.W skip     @0x12
  }
  // Dispatch block begins at offset 0x10 (word-compare path) or 0x16 (byte path).
  image.push_back(0x04U); image.push_back(0x00U); push16(image, 0x0005U);  // SUBI.B #5,D0
  image.push_back(0xE5U); image.push_back(0x48U);                          // LSL.W #2,D0
  const std::uint32_t jmp_off = static_cast<std::uint32_t>(image.size());
  image.push_back(0x4EU); image.push_back(0xFBU); push16(image, 0x0002U);  // JMP (2,PC,D0.W)
  const std::uint32_t table_off = jmp_off + 4U;                            // == ext-word addr (jmp_off+2) + 2
  for (int i = 0; i < 3; ++i) {                                            // 3 four-byte entries: RTS, NOP
    image.push_back(0x4EU); image.push_back(0x75U);
    image.push_back(0x4EU); image.push_back(0x71U);
  }
  while (image.size() < 0x2CU) image.push_back(0x71U);  // unreached filler, pad to skip label
  image.push_back(0x4EU); image.push_back(0x75U);                             // skip: RTS  @0x2C
  (void)table_off;
  return image;
}

void positive_bounded_selector_resolves_tier1() {
  using namespace segarecomp;
  const std::uint32_t base = 0x00002000U;
  const auto image = smps_dispatch_image(/*load_word=*/false, /*compare_word=*/false,
                                         /*signed_branch=*/false, /*upper_immediate=*/0x0007U);
  FlatImageDiscoveryEnvironment environment(image, base);
  const M68kStaticDiscoveryLimits limits{4096U, 4096U, 8U};
  const M68kProgramAddress entry{TargetAddressSpace::m68k_program, base};

  const auto result = discover_m68k_static_graph(entry, limits, environment);
  assert(!result.primary_issue.has_value());
  assert(result.indirect_target_ea_sets.size() == 1U);
  assert(result.unproven_indirect_control_ea_sets.empty());
  assert(result.ownerless_tier2_eligible_control_sources.empty());

  auto candidates = result.indirect_target_ea_sets.front().candidates;
  std::sort(candidates.begin(), candidates.end(),
            [](const M68kProgramAddress &a, const M68kProgramAddress &b) { return a.value < b.value; });
  assert(candidates.size() == 3U);
  // Selector D0 in {5,6,7} -> SUBI.B #5 -> {0,1,2} -> LSL.W #2 -> {0,4,8};
  // JMP target = pc_base + 2 + index, three contiguous four-byte entries.
  assert(candidates[1].value - candidates[0].value == 4U);
  assert(candidates[2].value - candidates[1].value == 4U);
  const auto lo = candidates[0].value;
  for (const auto &c : candidates)
    assert(std::any_of(result.block_entries.begin(), result.block_entries.end(),
                       [&](const M68kProgramAddress &e) { return e.value == c.value; }));
  assert(lo > base);
}

void positive_is_deterministic() {
  using namespace segarecomp;
  const std::uint32_t base = 0x00002000U;
  const auto image = smps_dispatch_image(false, false, false, 0x0007U);
  FlatImageDiscoveryEnvironment env_a(image, base);
  FlatImageDiscoveryEnvironment env_b(image, base);
  const M68kStaticDiscoveryLimits limits{4096U, 4096U, 8U};
  const M68kProgramAddress entry{TargetAddressSpace::m68k_program, base};
  const auto a = discover_m68k_static_graph(entry, limits, env_a);
  const auto b = discover_m68k_static_graph(entry, limits, env_b);
  assert(a.indirect_target_ea_sets.size() == b.indirect_target_ea_sets.size());
  assert(a.indirect_target_ea_sets.size() == 1U);
  assert(a.indirect_target_ea_sets.front().candidates.size() ==
         b.indirect_target_ea_sets.front().candidates.size());
}

// Inverse 1: a genuinely unknown selector (word load, no proven upper bits) with
// only a byte compare cannot be bounded -- a byte compare says nothing about
// bits 8-15 -- so the JMP stays Tier-2 unproven and discovery fails closed.
void negative_unknown_selector_byte_compare_stays_fail_closed() {
  using namespace segarecomp;
  const std::uint32_t base = 0x00002000U;
  const auto image = smps_dispatch_image(/*load_word=*/true, /*compare_word=*/false,
                                         /*signed_branch=*/false, /*upper_immediate=*/0x0007U);
  FlatImageDiscoveryEnvironment environment(image, base);
  const M68kStaticDiscoveryLimits limits{4096U, 4096U, 8U};
  const M68kProgramAddress entry{TargetAddressSpace::m68k_program, base};

  const auto result = discover_m68k_static_graph(entry, limits, environment);
  assert(result.indirect_target_ea_sets.empty());
  assert(result.unproven_indirect_control_ea_sets.size() == 1U);
  assert(result.primary_issue.has_value());
  assert(result.primary_issue->category == DirectFlowDiagnostic::reached_unresolved_direct_edge);
}

// Inverse 2: a word compare whose materialized interval would exceed the
// existing 256-member finite-value cap stays fail-closed.
void negative_word_compare_over_cap_stays_fail_closed() {
  using namespace segarecomp;
  const std::uint32_t base = 0x00002000U;
  const auto image = smps_dispatch_image(/*load_word=*/true, /*compare_word=*/true,
                                         /*signed_branch=*/false, /*upper_immediate=*/0x0400U);  // [0,1024]
  FlatImageDiscoveryEnvironment environment(image, base);
  const M68kStaticDiscoveryLimits limits{4096U, 4096U, 8U};
  const M68kProgramAddress entry{TargetAddressSpace::m68k_program, base};

  const auto result = discover_m68k_static_graph(entry, limits, environment);
  assert(result.indirect_target_ea_sets.empty());
  assert(result.unproven_indirect_control_ea_sets.size() == 1U);
  assert(result.primary_issue.has_value());
}

// Inverse 3: a signed branch condition is never consumed by the unsigned
// edge-refinement rule, so even a small word interval stays fail-closed.
void negative_signed_condition_not_consumed() {
  using namespace segarecomp;
  const std::uint32_t base = 0x00002000U;
  const auto image = smps_dispatch_image(/*load_word=*/true, /*compare_word=*/true,
                                         /*signed_branch=*/true, /*upper_immediate=*/0x0007U);
  FlatImageDiscoveryEnvironment environment(image, base);
  const M68kStaticDiscoveryLimits limits{4096U, 4096U, 8U};
  const M68kProgramAddress entry{TargetAddressSpace::m68k_program, base};

  const auto result = discover_m68k_static_graph(entry, limits, environment);
  assert(result.indirect_target_ea_sets.empty());
  assert(result.unproven_indirect_control_ea_sets.size() == 1U);
  assert(result.primary_issue.has_value());
}

// F1 (SEG-007-T199 adversarial finding): the `CMPI/CMP #imm,Dn` + `Bcc` edge
// constraint is sound only when the compare's flags provably executed on every
// path into the branch. Here the guard `Bcc` is ALSO a direct-branch target
// from a second site on which the compared `Dn` is finite but outside the
// tested interval, so the compare did not run on that path. The sole-in-edge
// guard must refuse to attach any constraint -> the selector merges to
// `unknown` and the computed `JMP` stays Tier-2 fail-closed. Without the guard
// the constraint would wrongly materialize a finite interval and resolve the
// `JMP` to a spurious target set.
void f1_branch_with_extra_in_edge_is_not_constrained() {
  using namespace segarecomp;
  const std::uint32_t base = 0x00002000U;
  std::vector<std::uint8_t> image;
  image.push_back(0x30U); image.push_back(0x38U); push16(image, 0x0002U);  // @0x00 MOVE.W ($2).W,D0
  image.push_back(0x66U); image.push_back(0x00U); push16(image, 0x0020U);  // @0x04 BNE.W L_other(0x26)
  image.push_back(0x0CU); image.push_back(0x40U); push16(image, 0x0007U);  // @0x08 CMPI.W #7,D0
  image.push_back(0x62U); image.push_back(0x00U); push16(image, 0x001EU);  // @0x0C BHI.W L_skip(0x2C)
  image.push_back(0x04U); image.push_back(0x40U); push16(image, 0x0004U);  // @0x10 SUBI.W #4,D0
  image.push_back(0xE5U); image.push_back(0x48U);                          // @0x14 LSL.W #2,D0
  image.push_back(0x4EU); image.push_back(0xFBU); push16(image, 0x0002U);  // @0x16 JMP (2,PC,D0.W)
  for (int i = 0; i < 3; ++i) {                                           // @0x1A table: 3x 4-byte
    image.push_back(0x4EU); image.push_back(0x75U);
    image.push_back(0x4EU); image.push_back(0x71U);
  }
  image.push_back(0x70U); image.push_back(0x04U);                          // @0x26 L_other: MOVEQ #4,D0
  image.push_back(0x60U); image.push_back(0x00U); push16(image, 0xFFE2U);  // @0x28 BRA.W L_bcc(0x0C)
  image.push_back(0x4EU); image.push_back(0x75U);                          // @0x2C L_skip: RTS

  FlatImageDiscoveryEnvironment environment(image, base);
  const M68kStaticDiscoveryLimits limits{4096U, 4096U, 8U};
  const M68kProgramAddress entry{TargetAddressSpace::m68k_program, base};
  const auto result = discover_m68k_static_graph(entry, limits, environment);

  assert(result.indirect_target_ea_sets.empty());
  assert(result.unproven_indirect_control_ea_sets.size() == 1U);
  assert(result.primary_issue.has_value());
}

// Positive coverage for the `CMP #imm,Dn` guard form and the `SUBQ #imm,Dn`
// normalization (the shared helper only exercises `CMPI` / `SUBI`).
void positive_cmp_immediate_and_subq_resolve_tier1() {
  using namespace segarecomp;
  const std::uint32_t base = 0x00002000U;
  std::vector<std::uint8_t> image;
  image.push_back(0x70U); image.push_back(0x00U);                          // @0x00 MOVEQ #0,D0
  image.push_back(0x10U); image.push_back(0x38U); push16(image, 0x0002U);  // @0x02 MOVE.B ($2).W,D0
  image.push_back(0xB0U); image.push_back(0x3CU); push16(image, 0x0005U);  // @0x06 CMP.B #5,D0
  image.push_back(0x65U); image.push_back(0x00U); push16(image, 0x001EU);  // @0x0A BCS.W L_skip(0x2A)
  image.push_back(0xB0U); image.push_back(0x3CU); push16(image, 0x0007U);  // @0x0E CMP.B #7,D0
  image.push_back(0x62U); image.push_back(0x00U); push16(image, 0x0016U);  // @0x12 BHI.W L_skip(0x2A)
  image.push_back(0x5BU); image.push_back(0x00U);                          // @0x16 SUBQ.B #5,D0
  image.push_back(0xE5U); image.push_back(0x48U);                          // @0x18 LSL.W #2,D0
  image.push_back(0x4EU); image.push_back(0xFBU); push16(image, 0x0002U);  // @0x1A JMP (2,PC,D0.W)
  for (int i = 0; i < 3; ++i) {                                           // @0x1E table: 3x 4-byte
    image.push_back(0x4EU); image.push_back(0x75U);
    image.push_back(0x4EU); image.push_back(0x71U);
  }
  image.push_back(0x4EU); image.push_back(0x75U);                          // @0x2A L_skip: RTS

  FlatImageDiscoveryEnvironment environment(image, base);
  const M68kStaticDiscoveryLimits limits{4096U, 4096U, 8U};
  const M68kProgramAddress entry{TargetAddressSpace::m68k_program, base};
  const auto result = discover_m68k_static_graph(entry, limits, environment);

  assert(!result.primary_issue.has_value());
  assert(result.unproven_indirect_control_ea_sets.empty());
  assert(result.indirect_target_ea_sets.size() == 1U);
  auto candidates = result.indirect_target_ea_sets.front().candidates;
  std::sort(candidates.begin(), candidates.end(),
            [](const M68kProgramAddress &a, const M68kProgramAddress &b) { return a.value < b.value; });
  assert(candidates.size() == 3U);
  // D0 in {5,6,7} -> SUBQ.B #5 -> {0,1,2} -> LSL.W #2 -> {0,4,8}; table at base+0x1E.
  assert(candidates[0].value == base + 0x1EU);
  assert(candidates[1].value == base + 0x22U);
  assert(candidates[2].value == base + 0x26U);
}

// The decoder resolves the quick shift-count field's architectural 0->8 mapping,
// so `LSL.W #0,D0` scales a finite selector by 8, not 0. Encoded count 0 with a
// {1} selector must produce {0x0100}; the downstream normalization then pins the
// `JMP` to the single table entry.
void decoder_lsl_quick_count_zero_means_eight() {
  using namespace segarecomp;
  const std::uint32_t base = 0x00002000U;
  std::vector<std::uint8_t> image;
  image.push_back(0x70U); image.push_back(0x01U);                          // @0x00 MOVEQ #1,D0
  image.push_back(0xE1U); image.push_back(0x48U);                          // @0x02 LSL.W #0,D0  (count 0 => 8)
  image.push_back(0x04U); image.push_back(0x40U); push16(image, 0x0100U);  // @0x04 SUBI.W #256,D0 -> {0}
  image.push_back(0xE5U); image.push_back(0x48U);                          // @0x08 LSL.W #2,D0 -> {0}
  image.push_back(0x4EU); image.push_back(0xFBU); push16(image, 0x0002U);  // @0x0A JMP (2,PC,D0.W)
  image.push_back(0x4EU); image.push_back(0x75U);                          // @0x0E table[0]: RTS
  image.push_back(0x4EU); image.push_back(0x71U);                          // @0x10 NOP filler

  FlatImageDiscoveryEnvironment environment(image, base);
  const M68kStaticDiscoveryLimits limits{4096U, 4096U, 8U};
  const M68kProgramAddress entry{TargetAddressSpace::m68k_program, base};
  const auto result = discover_m68k_static_graph(entry, limits, environment);

  assert(!result.primary_issue.has_value());
  assert(result.indirect_target_ea_sets.size() == 1U);
  const auto &candidates = result.indirect_target_ea_sets.front().candidates;
  assert(candidates.size() == 1U);
  assert(candidates.front().value == base + 0x0EU);
}

// SEG-021-T028 (Case C): a finite Tier-1 set is computed for a legal
// `JMP (d,PC,Dn.W)` but one candidate fails target admission. The finite set is
// discarded whole (no partial edges/blocks) and the source has exactly one
// owner, the existing Tier-2 fact -- never both tiers, never ownerless.
void case_c_rejected_finite_candidate_falls_back_to_tier2_only() {
  using namespace segarecomp;
  const std::uint32_t base = 0x00002000U;
  const auto image = smps_dispatch_image(/*load_word=*/false, /*compare_word=*/false,
                                         /*signed_branch=*/false, /*upper_immediate=*/0x0007U);
  const M68kStaticDiscoveryLimits limits{4096U, 4096U, 8U};
  const M68kProgramAddress entry{TargetAddressSpace::m68k_program, base};

  FlatImageDiscoveryEnvironment probe(image, base);
  const auto proven = discover_m68k_static_graph(entry, limits, probe);
  assert(proven.indirect_target_ea_sets.size() == 1U);
  const auto victim = proven.indirect_target_ea_sets.front().candidates[1].value;
  const auto edges_before = proven.edges.size();

  FlatImageDiscoveryEnvironment environment(image, base);
  environment.reject_call_target = victim;
  const auto result = discover_m68k_static_graph(entry, limits, environment);
  assert(result.indirect_target_ea_sets.empty());
  assert(result.unproven_indirect_control_ea_sets.size() == 1U);
  assert(result.unproven_indirect_control_ea_sets.front().control_ea.mode == M68kEaMode::pc_index8);
  assert(!result.unproven_indirect_control_ea_sets.front().is_call);
  assert(result.primary_issue.has_value());
  assert(result.primary_issue->category == DirectFlowDiagnostic::reached_unresolved_direct_edge);
  // No partial indirect edges from the rejected finite set leak on fallback.
  for (const auto &edge : result.edges) assert(edge.kind != M68kStaticEdgeKind::indirect_branch);
  assert(result.edges.size() < edges_before);
  assert(result.ownerless_tier2_eligible_control_sources.empty());
}

}  // namespace

int main() {
  case_c_rejected_finite_candidate_falls_back_to_tier2_only();
  positive_bounded_selector_resolves_tier1();
  positive_is_deterministic();
  negative_unknown_selector_byte_compare_stays_fail_closed();
  negative_word_compare_over_cap_stays_fail_closed();
  negative_signed_condition_not_consumed();
  f1_branch_with_extra_in_edge_is_not_constrained();
  positive_cmp_immediate_and_subq_resolve_tier1();
  decoder_lsl_quick_count_zero_means_eight();
  return 0;
}
