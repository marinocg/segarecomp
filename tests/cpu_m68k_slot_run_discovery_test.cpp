// SEG-151-T002 / ADR 0319: a `JMP (d8,PC,Dn.W)` whose index no finite-value proof bounds, preceded by `LSL.W #2,Dn` and entering a run of `BRA.W` slots in immutable cartridge bytes,
// is resolved to exactly that run (project-authored synthetic images only). Negatives: no scaling instruction, a single slot, a run ended by a non-slot word or an odd displacement,
// a run in bytes the environment does not call immutable, and an image whose table is not BRA.W slots (the pre-existing unproven behaviour is unchanged).
#include "segarecomp/cpu/m68k/static_discovery.hpp"

#include <cassert>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace {

class Env final : public segarecomp::M68kStaticDiscoveryEnvironment {
 public:
  Env(std::vector<std::uint8_t> image, std::uint32_t base, bool immutable) : image_(std::move(image)), base_(base), immutable_(immutable) {}
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
  std::optional<segarecomp::M68kMappingIssue> admit_target(segarecomp::M68kProgramAddress, segarecomp::M68kDiscoveryTargetRole) override { return std::nullopt; }
  std::optional<segarecomp::DirectFlowDiagnostic> classify_memory_access(const segarecomp::M68kCpuMemoryAccessRequest &) override { return std::nullopt; }
  bool is_completion_rts(const segarecomp::InstructionProvenance &) override { return false; }
  std::optional<segarecomp::M68kImmutableCartridgeBytes> read_immutable_cartridge_bytes(segarecomp::M68kProgramAddress address, std::uint32_t length) override {
    if (!immutable_ || address.value < base_ || address.value + length > base_ + image_.size()) return std::nullopt;
    segarecomp::M68kImmutableCartridgeBytes block{};
    const auto local = static_cast<std::size_t>(address.value - base_);
    block.bytes.assign(image_.begin() + static_cast<std::ptrdiff_t>(local), image_.begin() + static_cast<std::ptrdiff_t>(local + length));
    return block;
  }

 private:
  std::vector<std::uint8_t> image_;
  std::uint32_t base_;
  bool immutable_;
};

void w(std::vector<std::uint8_t> &image, std::uint32_t value) {
  image.push_back(static_cast<std::uint8_t>(value >> 8));
  image.push_back(static_cast<std::uint8_t>(value));
}

// @0 MOVE.W ($0002).W,D7 (unknown index)  @4 scale  @6 JMP 2(PC,D7.W)  @0A.. slots  then the filler and the three RTS targets at 0x20 / 0x22 / 0x24
std::vector<std::uint8_t> image(std::uint32_t scale, std::vector<std::uint32_t> slot_words, std::uint32_t after_run) {
  std::vector<std::uint8_t> out;
  w(out, 0x3E38); w(out, 0x0002);
  w(out, scale);
  w(out, 0x4EFB); w(out, 0x7002);
  for (const auto word : slot_words) w(out, word);
  w(out, after_run);
  while (out.size() < 0x20U) w(out, 0x4E71);
  w(out, 0x4E75); w(out, 0x4E75); w(out, 0x4E75);
  return out;
}


// A table of `n` BRA.W slots after `MOVE.W ($0002).W,D7 / <scale> / JMP 2(PC,D7.W)`, then `terminator` words (none = the image ends right after the last slot) and one RTS every slot branches to.
std::vector<std::uint8_t> sized(unsigned n, std::vector<std::uint32_t> terminator, std::uint32_t scale = 0xE54F, std::uint32_t ext = 0x7002) {
  std::vector<std::uint8_t> out;
  w(out, 0x3E38); w(out, 0x0002); w(out, scale); w(out, 0x4EFB); w(out, ext);
  const std::uint32_t target = 0x0AU + 4U * n + 2U * static_cast<std::uint32_t>(terminator.size()) + 2U;
  for (unsigned i = 0; i < n; ++i) { w(out, 0x6000); w(out, target - (0x0AU + 4U * i + 2U)); }
  for (const auto word : terminator) w(out, word);
  w(out, 0x4E71);
  w(out, 0x4E75);
  return out;
}

segarecomp::M68kStaticDiscoveryResult run(const std::vector<std::uint8_t> &bytes, bool immutable = true) {
  Env environment(bytes, 0x2000U, immutable);
  const segarecomp::M68kStaticDiscoveryLimits limits{4096U, 4096U, 8U};
  return segarecomp::discover_m68k_static_graph({segarecomp::TargetAddressSpace::m68k_program, 0x2000U}, limits, environment);
}

}  // namespace

int main() {
  // positive: three slots (0x2000 + 0x0A / 0x0E / 0x12)
  const auto good = image(0xE54F, {0x6000, 0x0014, 0x6000, 0x0012, 0x6000, 0x0010}, 0x4E71);
  const auto result = run(good);
  assert(!result.primary_issue.has_value());
  assert(result.indirect_target_ea_sets.size() == 1U && result.unproven_indirect_control_ea_sets.empty());
  const auto &set = result.indirect_target_ea_sets.front();
  assert(set.candidates.size() == 3U && set.candidates[0].value == 0x200AU && set.candidates[1].value == 0x200EU && set.candidates[2].value == 0x2012U);
  assert(set.index_values.values.size() == 3U && set.index_values.values[0] == 0U && set.index_values.values[1] == 4U && set.index_values.values[2] == 8U);

  // a finite proof that saw only one caller's constant (MOVEQ #0,D7) does not narrow the table: the whole run is the set
  {
    std::vector<std::uint8_t> narrow;
    w(narrow, 0x7E00); w(narrow, 0xE54F); w(narrow, 0x4EFB); w(narrow, 0x7002);
    for (const auto word : {0x6000U, 0x0016U, 0x6000U, 0x0014U, 0x6000U, 0x0012U}) w(narrow, word);
    w(narrow, 0x4E71);
    while (narrow.size() < 0x20U) w(narrow, 0x4E71);
    w(narrow, 0x4E75); w(narrow, 0x4E75); w(narrow, 0x4E75);
    const auto proven = run(narrow);
    assert(proven.indirect_target_ea_sets.size() == 1U && proven.indirect_target_ea_sets.front().candidates.size() == 3U);
  }


  // the bounded run: the extent must be ESTABLISHED (a readable terminator after the last slot, the 64th included)
  const auto sets = [](const segarecomp::M68kStaticDiscoveryResult &r) { return r.indirect_target_ea_sets.empty() ? 0U : r.indirect_target_ea_sets.front().candidates.size(); };
  assert(sets(run(sized(2, {0x4E71}))) == 2U);
  assert(sets(run(sized(63, {0x4E71}))) == 63U);
  assert(sets(run(sized(64, {0x4E71}))) == 64U);
  { const auto r = run(sized(65, {0x4E71})); assert(r.indirect_target_ea_sets.empty() && r.primary_issue.has_value()); }       // a 65th slot: not a proven run
  { const auto r = run(sized(100, {0x4E71})); assert(r.indirect_target_ea_sets.empty() && r.primary_issue.has_value()); }
  { std::vector<std::uint8_t> bytes = sized(3, {}); bytes.resize(0x0AU + 12U);      // the image ends right after the last slot: no readable terminator
    const auto r = run(bytes); assert(r.indirect_target_ea_sets.empty() && r.primary_issue.has_value()); }
  { std::vector<std::uint8_t> bytes = sized(64, {}); bytes.resize(0x0AU + 4U * 64U);
    const auto r = run(bytes); assert(r.indirect_target_ea_sets.empty() && r.primary_issue.has_value()); }                  // 64 slots, terminator unreadable
  assert(sets(run(sized(3, {0x6000, 0x0003}))) == 3U);                                                                    // an odd displacement terminates the run
  assert(sets(run(sized(3, {0x6000, 0x0000}))) == 3U);                                                                    // so does a zero displacement
  { const auto r = run(sized(3, {0x4E71}, 0x4E71)); assert(r.indirect_target_ea_sets.empty() && r.primary_issue.has_value()); }        // wrong scaling instruction
  { const auto r = run(sized(3, {0x4E71}, 0xE54E)); assert(r.indirect_target_ea_sets.empty() && r.primary_issue.has_value()); }        // wrong index register
  { const auto r = run(sized(3, {0x4E71}), false); assert(r.indirect_target_ea_sets.empty() && r.primary_issue.has_value()); }          // not immutable
  {   // the admitted set is exactly the run: an index outside it (e.g. 3 * 4 for a 3-slot table) is not a candidate, so the emitted guard stops on it at run time
    const auto r = run(sized(3, {0x4E71}));
    const auto &c = r.indirect_target_ea_sets.front().candidates;
    assert(c.size() == 3U && c.front().value == 0x200AU && c.back().value == 0x2012U);
    for (const auto &candidate : c) assert(candidate.value != 0x2016U);
  }

  // negatives: each keeps the unresolved (unproven, fail-closed) behaviour
  const auto no_scale = run(image(0x4E71, {0x6000, 0x0014, 0x6000, 0x0012, 0x6000, 0x0010}, 0x4E71));
  assert(no_scale.indirect_target_ea_sets.empty() && no_scale.primary_issue.has_value());
  const auto one_slot = run(image(0xE54F, {0x6000, 0x0014}, 0x4E71));
  assert(one_slot.indirect_target_ea_sets.empty() && one_slot.primary_issue.has_value());
  const auto wrong_register = run(image(0xE54E, {0x6000, 0x0014, 0x6000, 0x0012, 0x6000, 0x0010}, 0x4E71));   // LSL.W #2,D6, the index is D7
  assert(wrong_register.indirect_target_ea_sets.empty() && wrong_register.primary_issue.has_value());
  const auto odd_displacement = run(image(0xE54F, {0x6000, 0x0014, 0x6000, 0x0012, 0x6000, 0x0011}, 0x4E71));  // the third slot is not a slot: a run of two
  assert(odd_displacement.indirect_target_ea_sets.size() == 1U && odd_displacement.indirect_target_ea_sets.front().candidates.size() == 2U);
  const auto not_immutable = run(good, /*immutable=*/false);
  assert(not_immutable.indirect_target_ea_sets.empty() && not_immutable.primary_issue.has_value());
  const auto not_slots = run(image(0xE54F, {0x4E75, 0x4E75, 0x4E75, 0x4E75}, 0x4E71));
  assert(not_slots.indirect_target_ea_sets.empty() && not_slots.primary_issue.has_value());
  return 0;
}
