// SEG-007-T181 / ADR-0027: focused coverage for the CPU-owned
// fallthrough-continuation partition seam in `discover_m68k_static_graph`.
//
// The walker exposes a `fallthrough_continuation_frontier` for every ceiling
// trip that lands on a cleanly-decoding `next_pc` step (sequential fallthrough
// or branch/call continuation), and, when that address is passed in the
// SEPARATE `fallthrough_continuation_boundaries` set, records a distinct
// `fallthrough_continuation` edge + block entry and does NOT walk the body.
// A ceiling trip reached only via a control-transfer target is NOT eligible
// and keeps its existing fatal `discovery_budget_exhausted` diagnostic.

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

// A long linear MOVEQ stretch followed by RTS. MOVEQ #0,D0 is `0x70 0x00`
// (2 bytes, a plain straight-line advance in the discovery whitelist).
std::vector<std::uint8_t> linear_moveq_stretch(std::uint32_t moveq_count) {
  std::vector<std::uint8_t> image;
  for (std::uint32_t i = 0; i < moveq_count; ++i) {
    image.push_back(0x70U);
    image.push_back(0x00U);
  }
  image.push_back(0x4EU);
  image.push_back(0x75U);  // RTS
  return image;
}

// Case A: a ceiling trip on a sequential-fallthrough `next_pc` step whose
// continuation decodes cleanly is reported in `fallthrough_continuation_frontier`
// (and still produces the primary `discovery_budget_exhausted`). No continuation
// boundary set supplied.
void ceiling_trip_on_clean_next_pc_is_reported_as_continuation_frontier() {
  using namespace segarecomp;
  const std::uint32_t base = 0x00002000U;
  const auto image = linear_moveq_stretch(8U);
  FlatImageDiscoveryEnvironment environment(image, base);
  const M68kStaticDiscoveryLimits limits{3U, 192U, 2U};  // trips after 3 instructions
  const M68kProgramAddress entry{TargetAddressSpace::m68k_program, base};

  const auto result = discover_m68k_static_graph(entry, limits, environment);
  assert(result.primary_issue.has_value());
  assert(result.primary_issue->category == DirectFlowDiagnostic::discovery_budget_exhausted);
  // 3 instructions consumed at base, base+2, base+4; the ceiling trip is the
  // next_pc at base+6.
  assert(contains_address(result.fallthrough_continuation_frontier, base + 6U));
  assert(result.stitched_fallthrough_continuation_edges == 0U);
}

// Case B: with the ceiling-trip continuation address in the SEPARATE boundary
// set, the walk records exactly one `fallthrough_continuation` edge + block
// entry, does NOT walk the continuation body, and completes without a primary
// issue. The decoded prefix is identical to a raised-ceiling monolithic walk.
void continuation_boundary_elides_body_and_records_one_distinct_edge() {
  using namespace segarecomp;
  const std::uint32_t base = 0x00002000U;
  const auto image = linear_moveq_stretch(8U);
  FlatImageDiscoveryEnvironment environment(image, base);
  const M68kProgramAddress entry{TargetAddressSpace::m68k_program, base};
  const std::uint32_t split = base + 6U;

  const M68kStaticDiscoveryLimits tight{3U, 192U, 2U};
  const auto stitched = discover_m68k_static_graph(entry, tight, environment, {}, std::set<std::uint32_t>{split});
  assert(!stitched.primary_issue.has_value());
  assert(stitched.stitched_fallthrough_continuation_edges == 1U);
  assert(contains_address(stitched.block_entries, split));
  assert(!contains_address(stitched.decode_order, split));  // body not walked
  const bool have_edge = std::any_of(stitched.edges.begin(), stitched.edges.end(), [&](const M68kStaticEdge &e) {
    return e.kind == M68kStaticEdgeKind::fallthrough_continuation && e.target.value == split &&
           e.source_instruction.source.address.value == base + 4U;
  });
  assert(have_edge);

  // Raised-ceiling monolithic walk: the decoded prefix below `split` matches.
  const M68kStaticDiscoveryLimits raised{256U, 192U, 2U};
  const auto monolith = discover_m68k_static_graph(entry, raised, environment);
  assert(!monolith.primary_issue.has_value());
  for (const auto &decoded : stitched.decode_order) {
    assert(decoded.value < split);
    assert(contains_address(monolith.decode_order, decoded.value));
  }
  // No instruction added or dropped below the split.
  std::uint32_t monolith_prefix = 0U;
  for (const auto &d : monolith.decode_order)
    if (d.value < split) ++monolith_prefix;
  assert(monolith_prefix == stitched.decode_order.size());
}

// Case C: a ceiling trip reached only via a control-transfer target (a taken
// branch destination, never a `next_pc` step) is NOT eligible for continuation
// and still fails closed with the existing frontier diagnostic.
void ceiling_trip_on_open_control_edge_is_not_continuation_eligible() {
  using namespace segarecomp;
  const std::uint32_t base = 0x00002000U;
  std::vector<std::uint8_t> image(0x20U, 0x70U);  // filler MOVEQ bytes
  for (std::size_t i = 1; i < image.size(); i += 2) image[i] = 0x00U;
  image[0x00] = 0x60U; image[0x01] = 0x00U; image[0x02] = 0x00U; image[0x03] = 0x10U;  // BRA.W base+0x12
  FlatImageDiscoveryEnvironment environment(image, base);
  const M68kStaticDiscoveryLimits limits{1U, 192U, 2U};  // trips right after the BRA
  const M68kProgramAddress entry{TargetAddressSpace::m68k_program, base};

  const auto result = discover_m68k_static_graph(entry, limits, environment);
  assert(result.primary_issue.has_value());
  assert(result.primary_issue->category == DirectFlowDiagnostic::discovery_budget_exhausted);
  assert(result.fallthrough_continuation_frontier.empty());
  assert(result.stitched_fallthrough_continuation_edges == 0U);
}

// Case D: the exposed continuation frontier is deterministic across runs.
void continuation_frontier_is_deterministic() {
  using namespace segarecomp;
  const std::uint32_t base = 0x00002000U;
  const auto image = linear_moveq_stretch(12U);
  FlatImageDiscoveryEnvironment environment_a(image, base);
  FlatImageDiscoveryEnvironment environment_b(image, base);
  const M68kStaticDiscoveryLimits limits{5U, 192U, 2U};
  const M68kProgramAddress entry{TargetAddressSpace::m68k_program, base};

  const auto a = discover_m68k_static_graph(entry, limits, environment_a);
  const auto b = discover_m68k_static_graph(entry, limits, environment_b);
  assert(a.fallthrough_continuation_frontier.size() == b.fallthrough_continuation_frontier.size());
  for (std::size_t i = 0; i < a.fallthrough_continuation_frontier.size(); ++i)
    assert(a.fallthrough_continuation_frontier[i].value == b.fallthrough_continuation_frontier[i].value);
}

}  // namespace

int main() {
  ceiling_trip_on_clean_next_pc_is_reported_as_continuation_frontier();
  continuation_boundary_elides_body_and_records_one_distinct_edge();
  ceiling_trip_on_open_control_edge_is_not_continuation_eligible();
  continuation_frontier_is_deterministic();
  return 0;
}
