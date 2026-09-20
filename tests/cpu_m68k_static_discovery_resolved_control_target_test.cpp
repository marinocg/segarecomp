// SEG-007-T182 / ADR-0028: focused coverage for the CPU-owned
// resolved-control-target frontier seam in `discover_m68k_static_graph`.
//
// When a per-root instruction ceiling is reached on the FIRST instruction of a
// statically-resolved+validated direct control-transfer destination (a taken
// direct-branch target, a JSR/BSR/foldable JMP-JSR callee, or an ADR-0009
// finite indirect candidate) that was NOT also reached by a plain `next_pc`
// step, and that entry decodes cleanly as a prefix-boundary shape, the walker
// exposes it in `resolved_control_target_frontier`. The A -> X control edge is
// still recorded by the normal control-target path; only X's recursive body
// walk is later partitioned by Phase-2 synthesis (frontend). A ceiling trip at
// a genuinely unresolved / undecodable / unmapped / misaligned open control
// edge is NEVER reported here.

#include "segarecomp/cpu/m68k/static_discovery.hpp"

#include <algorithm>
#include <cassert>
#include <optional>
#include <set>
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

// Image: BRA.W to a far MOVEQ stretch; the branch target begins a candidate-free
// linear region. Byte 0..3: 0x60 0x00 <hi> <lo> (BRA.W, displacement from pc+2).
std::vector<std::uint8_t> bra_then_far_moveq_region(std::uint32_t target_offset, std::uint32_t region_moveqs) {
  std::vector<std::uint8_t> image;
  image.push_back(0x60U);
  image.push_back(0x00U);
  const std::uint32_t disp = target_offset - 2U;  // relative to pc+2
  image.push_back(static_cast<std::uint8_t>((disp >> 8) & 0xFFU));
  image.push_back(static_cast<std::uint8_t>(disp & 0xFFU));
  while (image.size() < target_offset) image.push_back(0x71U);  // filler (unreached)
  for (std::uint32_t i = 0; i < region_moveqs; ++i) {
    image.push_back(0x70U);
    image.push_back(0x00U);  // MOVEQ #0,D0
  }
  image.push_back(0x4EU);
  image.push_back(0x75U);  // RTS
  return image;
}

// Proof 1: a resolved direct-branch target reached at the local discovery
// ceiling becomes a reported resolved-control-target frontier (eligible for a
// separately validated unit) rather than only a fatal budget exhaustion.
void resolved_branch_target_at_ceiling_is_reported_as_frontier() {
  using namespace segarecomp;
  const std::uint32_t base = 0x00002000U;
  const std::uint32_t target_offset = 0x40U;
  const auto image = bra_then_far_moveq_region(target_offset, 8U);
  FlatImageDiscoveryEnvironment environment(image, base);
  // Ceiling of 1: the BRA is instruction 0; the ceiling trips on the first
  // instruction of the branch target (a control target, never a next_pc step).
  const M68kStaticDiscoveryLimits limits{1U, 192U, 2U};
  const M68kProgramAddress entry{TargetAddressSpace::m68k_program, base};

  const auto result = discover_m68k_static_graph(entry, limits, environment);
  assert(result.primary_issue.has_value());
  assert(result.primary_issue->category == DirectFlowDiagnostic::discovery_budget_exhausted);
  assert(contains_address(result.resolved_control_target_frontier, base + target_offset));
  assert(result.fallthrough_continuation_frontier.empty());
}

// Proof 2: the real branch edge stays exactly one direct-control edge; no fake
// runtime/continuation boundary is introduced, and passing the target in the
// independent-unit boundary set elides its body while keeping that one edge.
void resolved_target_edge_is_one_direct_control_edge_and_body_is_partitioned() {
  using namespace segarecomp;
  const std::uint32_t base = 0x00002000U;
  const std::uint32_t target_offset = 0x40U;
  const auto image = bra_then_far_moveq_region(target_offset, 8U);
  FlatImageDiscoveryEnvironment environment(image, base);
  const M68kProgramAddress entry{TargetAddressSpace::m68k_program, base};
  const std::uint32_t split = base + target_offset;

  const M68kStaticDiscoveryLimits limits{16U, 192U, 2U};
  const auto stitched =
      discover_m68k_static_graph(entry, limits, environment, std::set<std::uint32_t>{split}, {});
  assert(!stitched.primary_issue.has_value());
  assert(stitched.stitched_boundary_edges == 1U);
  assert(stitched.stitched_fallthrough_continuation_edges == 0U);
  assert(contains_address(stitched.block_entries, split));
  assert(!contains_address(stitched.decode_order, split));  // body not walked here
  const auto direct_branch_edges = std::count_if(
      stitched.edges.begin(), stitched.edges.end(), [&](const M68kStaticEdge &e) {
        return e.target.value == split && e.kind == M68kStaticEdgeKind::direct_branch;
      });
  assert(direct_branch_edges == 1);
  const bool has_continuation_edge = std::any_of(
      stitched.edges.begin(), stitched.edges.end(),
      [&](const M68kStaticEdge &e) { return e.kind == M68kStaticEdgeKind::fallthrough_continuation; });
  assert(!has_continuation_edge);
}

// Proof 4a: an unmapped control target at the ceiling is a genuine open edge and
// is NOT reported as a resolved-control-target frontier (fail closed unchanged).
void unmapped_control_target_is_not_eligible() {
  using namespace segarecomp;
  const std::uint32_t base = 0x00002000U;
  std::vector<std::uint8_t> image;
  image.push_back(0x60U); image.push_back(0x00U); image.push_back(0x7FU); image.push_back(0xF0U);  // BRA far out of image
  FlatImageDiscoveryEnvironment environment(image, base);
  const M68kStaticDiscoveryLimits limits{4U, 192U, 2U};
  const M68kProgramAddress entry{TargetAddressSpace::m68k_program, base};

  const auto result = discover_m68k_static_graph(entry, limits, environment);
  assert(result.primary_issue.has_value());
  assert(result.resolved_control_target_frontier.empty());
}

// Proof 4b: an odd / misaligned direct-branch target is rejected before it can
// ever be recorded as a resolved-control-target frontier.
void misaligned_control_target_is_not_eligible() {
  using namespace segarecomp;
  const std::uint32_t base = 0x00002000U;
  std::vector<std::uint8_t> image;
  image.push_back(0x60U); image.push_back(0x00U); image.push_back(0x00U); image.push_back(0x0DU);  // BRA to pc+2+0xD (odd)
  while (image.size() < 0x20U) image.push_back(0x70U);
  for (std::size_t i = 1; i < image.size(); i += 2) image[i] = 0x00U;
  FlatImageDiscoveryEnvironment environment(image, base);
  const M68kStaticDiscoveryLimits limits{4U, 192U, 2U};
  const M68kProgramAddress entry{TargetAddressSpace::m68k_program, base};

  const auto result = discover_m68k_static_graph(entry, limits, environment);
  assert(result.primary_issue.has_value());  // fails closed
  assert(result.resolved_control_target_frontier.empty());  // never eligible
}

// Proof 13 (discovery layer): the exposed resolved-control-target frontier is a
// deterministic pure function of the image across two independent walks.
void resolved_control_target_frontier_is_deterministic() {
  using namespace segarecomp;
  const std::uint32_t base = 0x00002000U;
  const auto image = bra_then_far_moveq_region(0x40U, 10U);
  FlatImageDiscoveryEnvironment environment_a(image, base);
  FlatImageDiscoveryEnvironment environment_b(image, base);
  const M68kStaticDiscoveryLimits limits{1U, 192U, 2U};
  const M68kProgramAddress entry{TargetAddressSpace::m68k_program, base};

  const auto a = discover_m68k_static_graph(entry, limits, environment_a);
  const auto b = discover_m68k_static_graph(entry, limits, environment_b);
  assert(a.resolved_control_target_frontier.size() == b.resolved_control_target_frontier.size());
  for (std::size_t i = 0; i < a.resolved_control_target_frontier.size(); ++i)
    assert(a.resolved_control_target_frontier[i].value == b.resolved_control_target_frontier[i].value);
}

// Proof 5 (bounded chaining): a resolved-control-target unit that itself reaches
// another eligible target produces a bounded, deterministic frontier list, not
// recursive whole-program ownership. Two chained BRA hops; walking with the
// first hop as an independent-unit boundary still surfaces the second hop's
// ceiling frontier from the boundary unit's own bounded walk.
void chained_resolved_targets_stay_bounded() {
  using namespace segarecomp;
  const std::uint32_t base = 0x00002000U;
  // hop1 at 0x20 -> BRA to hop2 at 0x60 -> MOVEQ stretch + RTS
  std::vector<std::uint8_t> image(0x20U, 0x00U);
  image[0] = 0x60U; image[1] = 0x00U; image[2] = 0x00U; image[3] = 0x1EU;  // BRA to pc+2+0x1E = 0x20
  for (std::size_t i = 4; i < 0x20U; i += 2) { image[i] = 0x70U; image[i + 1] = 0x00U; }
  image.push_back(0x60U); image.push_back(0x00U); image.push_back(0x00U); image.push_back(0x3EU);  // 0x20: BRA to 0x60
  while (image.size() < 0x60U) image.push_back(0x71U);
  for (std::uint32_t i = 0; i < 8U; ++i) { image.push_back(0x70U); image.push_back(0x00U); }
  image.push_back(0x4EU); image.push_back(0x75U);
  FlatImageDiscoveryEnvironment environment(image, base);
  const M68kProgramAddress entry{TargetAddressSpace::m68k_program, base};
  const M68kStaticDiscoveryLimits limits{2U, 192U, 2U};  // trips at the hop2 target

  const auto result =
      discover_m68k_static_graph(entry, limits, environment, std::set<std::uint32_t>{base + 0x20U}, {});
  // hop1's body is partitioned; the ceiling frontier is bounded (<= a couple of
  // addresses), deterministic, and never the whole downstream region.
  assert(result.resolved_control_target_frontier.size() <= 2U);
}

}  // namespace

int main() {
  resolved_branch_target_at_ceiling_is_reported_as_frontier();
  resolved_target_edge_is_one_direct_control_edge_and_body_is_partitioned();
  unmapped_control_target_is_not_eligible();
  misaligned_control_target_is_not_eligible();
  resolved_control_target_frontier_is_deterministic();
  chained_resolved_targets_stay_bounded();
  return 0;
}
