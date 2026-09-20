#include "segarecomp/cpu/m68k/static_discovery.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <optional>
#include <set>
#include <span>
#include <tuple>
#include <vector>

namespace {

// SEG-007-T180 / ADR-0026: a minimal flat-image discovery environment for the
// offline-inventory boundary-guard tests below. It admits every mapped
// direct target and classifies every foldable access, so the only behaviour
// under test is the CPU-owned traversal's `independent_unit_boundaries`
// stitch: a statically-resolved control-transfer destination that is a
// boundary member has its edge/frame/block-entry recorded but is NOT walked
// in this pass's budget, while sequential fallthrough and call/branch
// continuation are never guarded.
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
  std::optional<segarecomp::M68kMappingIssue> admit_target(
      segarecomp::M68kProgramAddress, segarecomp::M68kDiscoveryTargetRole) override {
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

bool contains_address(const std::vector<segarecomp::M68kProgramAddress> &addresses, std::uint32_t value) {
  return std::any_of(addresses.begin(), addresses.end(),
                     [&](const segarecomp::M68kProgramAddress &a) { return a.value == value; });
}

bool has_edge(const std::vector<segarecomp::M68kStaticEdge> &edges, segarecomp::M68kStaticEdgeKind kind,
              std::uint32_t source, std::uint32_t target) {
  return std::any_of(edges.begin(), edges.end(), [&](const segarecomp::M68kStaticEdge &e) {
    return e.kind == kind && e.source_instruction.source.address.value == source && e.target.value == target;
  });
}

// SEG-007-T180 / ADR-0026 case 1: a direct (unconditional) branch A->B where B
// is an admitted offline-inventory unit boundary. The A->B edge and B's
// block entry are recorded, B is NOT decoded inside A's own walk budget, and
// with an empty boundary set (every pre-T180 caller) B *is* walked -- so the
// boundary set alone accounts for the difference and never changes the
// recorded edge/block facts.
void t180_direct_branch_to_unit_boundary_records_edge_without_walking_the_body() {
  using namespace segarecomp;
  const std::uint32_t base = 0x00002000U;
  std::vector<std::uint8_t> image(0x104U, 0U);
  image[0x000] = 0x60U; image[0x001] = 0x00U; image[0x002] = 0x00U; image[0x003] = 0xFEU;  // 0x2000 BRA.W 0x2100
  image[0x100] = 0x70U; image[0x101] = 0x07U;  // 0x2100 MOVEQ #7,D0
  image[0x102] = 0x4EU; image[0x103] = 0x75U;  // 0x2102 RTS
  FlatImageDiscoveryEnvironment environment(image, base);
  const M68kStaticDiscoveryLimits limits{256U, 192U, 2U};
  const M68kProgramAddress entry{TargetAddressSpace::m68k_program, base};

  const auto stitched = discover_m68k_static_graph(entry, limits, environment, std::set<std::uint32_t>{0x00002100U});
  assert(stitched.stitched_boundary_edges == 1U);
  assert(contains_address(stitched.decode_order, 0x00002000U));
  assert(!contains_address(stitched.decode_order, 0x00002100U));
  assert(!contains_address(stitched.decode_order, 0x00002102U));
  assert(contains_address(stitched.block_entries, 0x00002100U));
  assert(has_edge(stitched.edges, M68kStaticEdgeKind::direct_branch, 0x00002000U, 0x00002100U));
  assert(!stitched.primary_issue.has_value());

  const auto whole = discover_m68k_static_graph(entry, limits, environment);
  assert(whole.stitched_boundary_edges == 0U);
  assert(contains_address(whole.decode_order, 0x00002100U));
  assert(contains_address(whole.decode_order, 0x00002102U));
  assert(whole.decode_order.size() > stitched.decode_order.size());
  assert(has_edge(whole.edges, M68kStaticEdgeKind::direct_branch, 0x00002000U, 0x00002100U));
}

// SEG-007-T180 / ADR-0026 case 2: a direct call A->B where B is an admitted
// unit boundary. The call frame and its callee/continuation identity are
// recorded, the caller continuation block is still walked (continuation is
// never stitched), and B's body is not decoded in A's budget.
void t180_direct_call_to_unit_boundary_keeps_continuation_but_stitches_the_callee() {
  using namespace segarecomp;
  const std::uint32_t base = 0x00002000U;
  std::vector<std::uint8_t> image(0x104U, 0U);
  image[0x000] = 0x4EU; image[0x001] = 0xB9U;                                        // 0x2000 JSR (xxx).L
  image[0x002] = 0x00U; image[0x003] = 0x00U; image[0x004] = 0x21U; image[0x005] = 0x00U;  //        -> 0x00002100
  image[0x006] = 0x70U; image[0x007] = 0x09U;  // 0x2006 MOVEQ #9,D0 (continuation block)
  image[0x008] = 0x4EU; image[0x009] = 0x75U;  // 0x2008 RTS
  image[0x100] = 0x70U; image[0x101] = 0x01U;  // 0x2100 MOVEQ #1,D0 (callee unit body)
  image[0x102] = 0x4EU; image[0x103] = 0x75U;  // 0x2102 RTS
  FlatImageDiscoveryEnvironment environment(image, base);
  const M68kStaticDiscoveryLimits limits{256U, 192U, 2U};
  const M68kProgramAddress entry{TargetAddressSpace::m68k_program, base};

  const auto stitched = discover_m68k_static_graph(entry, limits, environment, std::set<std::uint32_t>{0x00002100U});
  assert(stitched.stitched_boundary_edges == 1U);
  assert(contains_address(stitched.decode_order, 0x00002006U));   // continuation still walked
  assert(!contains_address(stitched.decode_order, 0x00002100U));  // callee body stitched, not walked
  assert(contains_address(stitched.block_entries, 0x00002100U));
  assert(contains_address(stitched.block_entries, 0x00002006U));
  const bool have_frame = std::any_of(stitched.frames.begin(), stitched.frames.end(), [](const M68kStaticFrame &f) {
    return f.call.callee.value == 0x00002100U && f.call.continuation.value == 0x00002006U;
  });
  assert(have_frame);
  assert(has_edge(stitched.edges, M68kStaticEdgeKind::direct_call, 0x00002000U, 0x00002100U));

  const auto whole = discover_m68k_static_graph(entry, limits, environment);
  assert(whole.stitched_boundary_edges == 0U);
  assert(contains_address(whole.decode_order, 0x00002100U));
}

// SEG-007-T180 / ADR-0026 case 3: a conditional branch whose local fallthrough
// successor is walked while its taken successor (an admitted unit boundary)
// is stitched. BOTH edges are present.
void t180_conditional_branch_walks_fallthrough_and_stitches_the_taken_unit_boundary() {
  using namespace segarecomp;
  const std::uint32_t base = 0x00002000U;
  std::vector<std::uint8_t> image(0x104U, 0U);
  image[0x000] = 0x66U; image[0x001] = 0x00U; image[0x002] = 0x00U; image[0x003] = 0xFEU;  // 0x2000 BNE.W 0x2100
  image[0x004] = 0x74U; image[0x005] = 0x02U;  // 0x2004 MOVEQ #2,D2 (fallthrough body)
  image[0x006] = 0x4EU; image[0x007] = 0x75U;  // 0x2006 RTS
  image[0x100] = 0x76U; image[0x101] = 0x03U;  // 0x2100 MOVEQ #3,D3 (taken unit body)
  image[0x102] = 0x4EU; image[0x103] = 0x75U;  // 0x2102 RTS
  FlatImageDiscoveryEnvironment environment(image, base);
  const M68kStaticDiscoveryLimits limits{256U, 192U, 2U};
  const M68kProgramAddress entry{TargetAddressSpace::m68k_program, base};

  const auto stitched = discover_m68k_static_graph(entry, limits, environment, std::set<std::uint32_t>{0x00002100U});
  assert(stitched.stitched_boundary_edges == 1U);
  assert(contains_address(stitched.decode_order, 0x00002004U));   // fallthrough body walked
  assert(contains_address(stitched.decode_order, 0x00002006U));
  assert(!contains_address(stitched.decode_order, 0x00002100U));  // taken unit body stitched
  assert(has_edge(stitched.edges, M68kStaticEdgeKind::fallthrough, 0x00002000U, 0x00002004U));
  assert(has_edge(stitched.edges, M68kStaticEdgeKind::direct_branch, 0x00002000U, 0x00002100U));
  assert(!stitched.primary_issue.has_value());
}

// SEG-007-T180 / ADR-0026 case 4: an ordinary sequential fallthrough address
// that also appears in the boundary set is NOT a stitch boundary -- the
// linear instruction stream is never truncated and no stitch is counted.
void t180_sequential_fallthrough_is_never_a_stitch_boundary() {
  using namespace segarecomp;
  const std::uint32_t base = 0x00002000U;
  std::vector<std::uint8_t> image{
      0x70U, 0x00U,  // 0x2000 MOVEQ #0,D0
      0x72U, 0x01U,  // 0x2002 MOVEQ #1,D1
      0x74U, 0x02U,  // 0x2004 MOVEQ #2,D2
      0x4EU, 0x75U,  // 0x2006 RTS
  };
  FlatImageDiscoveryEnvironment environment(image, base);
  const M68kStaticDiscoveryLimits limits{256U, 192U, 2U};
  const M68kProgramAddress entry{TargetAddressSpace::m68k_program, base};

  const auto result = discover_m68k_static_graph(entry, limits, environment, std::set<std::uint32_t>{0x00002002U});
  assert(result.stitched_boundary_edges == 0U);
  assert(contains_address(result.decode_order, 0x00002000U));
  assert(contains_address(result.decode_order, 0x00002002U));
  assert(contains_address(result.decode_order, 0x00002004U));
  assert(contains_address(result.decode_order, 0x00002006U));
  assert(!result.primary_issue.has_value());

  // Total no-op: an empty boundary set is byte-identical to the guarded call
  // with a fallthrough-only boundary here (fallthrough is never guarded).
  const auto baseline = discover_m68k_static_graph(entry, limits, environment);
  assert(baseline.decode_order.size() == result.decode_order.size());
  assert(baseline.stitched_boundary_edges == 0U);
}

}  // namespace

int main() {
  using namespace segarecomp;
  // Project-authored MOVEQ bytes exercise the CPU-only source/decode/cache
  // seam; no machine mapping or device policy is needed to establish it.
  const std::array<std::uint8_t, 2> bytes{0x70U, 0x01U};
  const DecodeSource source{CpuVariant::mc68000,
                            {TargetAddressSpace::m68k_program, 0x100U}, {0U}};
  M68kStaticDecodeCache cache;
  const M68kInstructionSource instruction{bytes, source, {0x40U}};
  const auto first = cache.decode_or_get(instruction, M68kDecodeProfile::general_startup);
  assert(std::holds_alternative<M68kDecodedInstruction>(first));
  assert(std::get<M68kDecodedInstruction>(first).provenance.source.image_offset.value == 0x40U);
  assert(cache.entries().size() == 1U);
  const auto second = cache.decode_or_get(instruction, M68kDecodeProfile::general_startup);
  assert(std::holds_alternative<M68kDecodedInstruction>(second));
  assert(cache.entries().size() == 1U);

  const std::array<std::uint8_t, 1> truncated_bytes{0x70U};
  const M68kInstructionSource truncated{truncated_bytes, source, {0x40U}};
  M68kStaticDecodeCache truncated_cache;
  const auto rejected = truncated_cache.decode_or_get(truncated, M68kDecodeProfile::general_startup);
  assert(std::holds_alternative<M68kDiscoveryDecodeIssue>(rejected));
  const auto &issue = std::get<M68kDiscoveryDecodeIssue>(rejected);
  assert(issue.kind == M68kDiscoveryDecodeIssueKind::truncated_instruction);
  assert(issue.address.space == source.address.space);
  assert(issue.address.value == source.address.value);
  assert(issue.available_bytes == 1U);

  // SEG-007-T252 / ADR-0040 correction: the former SEG-007-T150/ADR-0016
  // static finite-loop-progress proof and SEG-007-T155/ADR-0017 (extended by
  // ADR-0018/ADR-0019) generated data-transform progress proof test coverage
  // was removed in this task, along with their now-dead CPU-side producers
  // (see ADR-0040 section 7 and libs/cpu/m68k/src/static_loop_proof.cpp's own
  // removal). Every OTHER static-discovery fact this file tests (decode
  // cache and the SEG-007-T180 offline-inventory boundary-guard stitch
  // below) is preserved exactly.
  // SEG-007-T180 / ADR-0026: offline-inventory boundary-guard stitch. Full
  // refinement owns cases 5/6/8/9 (overlap merge / cross-unit fail-closed /
  // Tier-1 regression / Tier-2 dispatch) since it may reshape aggregation.
  t180_direct_branch_to_unit_boundary_records_edge_without_walking_the_body();
  t180_direct_call_to_unit_boundary_keeps_continuation_but_stitches_the_callee();
  t180_conditional_branch_walks_fallthrough_and_stitches_the_taken_unit_boundary();
  t180_sequential_fallthrough_is_never_a_stitch_boundary();

  return 0;
}
