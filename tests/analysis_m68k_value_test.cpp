// SEG-030-T003 (ADR 0079): the CPU-owned M68K address domain (region + offset, points-to for A0-A7) and the address-register
// control families `JSR/JMP (An)`, `d16(An)` and `(d8,An,Xn)`.
//
// Project-authored synthetic fixtures only: real MC68000 encodings decoded by the unchanged decoder/lifter over a small flat
// cartridge image plus a work-RAM region extent, so exact synthetic addresses may be asserted. No commercial input.

#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "segarecomp/cpu/m68k/analysis/address_value.hpp"
#include "segarecomp/cpu/m68k/analysis/finite_adapter.hpp"

namespace {

using namespace segarecomp;
using analysis::UnknownReason;
using Sub = M68kAnalysisSubReason;

int failures = 0;
void expect(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

constexpr std::uint32_t entry = 0x200U;
constexpr std::uint32_t image_size = 0x1000U;
constexpr std::uint32_t work_ram_base = 0xE00000U;

struct Image {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(image_size, 0U);
  void put32(std::uint32_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4U; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (24U - 8U * i));
  }
  void words(std::uint32_t at, std::initializer_list<std::uint32_t> values) {
    for (const auto value : values) {
      bytes[at] = static_cast<std::uint8_t>(value >> 8U);
      bytes[at + 1U] = static_cast<std::uint8_t>(value);
      at += 2U;
    }
  }
};

// The flat cartridge (one immutable image region) plus a work-RAM extent that is neither executable nor immutable.
class RegionImage final : public M68kAnalysisImage {
public:
  explicit RegionImage(const Image &image) : flat_(image.bytes, 0U) {}
  [[nodiscard]] std::optional<Instruction> decode(std::uint32_t pc) const override { return flat_.decode(pc); }
  [[nodiscard]] bool mapped(std::uint32_t pc) const override { return flat_.mapped(pc); }
  [[nodiscard]] std::optional<std::uint32_t> immutable_read(std::uint32_t address, unsigned bytes) const override {
    return flat_.immutable_read(address, bytes);
  }
  [[nodiscard]] std::optional<M68kRegionExtent> region_of(std::uint32_t address) const override {
    if (address >= work_ram_base && address < 0x1000000U)
      return M68kRegionExtent{M68kRegionKind::work_ram, 0U, work_ram_base, 0x1000000U - work_ram_base};
    return flat_.region_of(address);
  }

private:
  M68kFlatAnalysisImage flat_;
};

M68kFiniteAnalysisResult run(const Image &image, bool address, bool accept_width = false) {
  const RegionImage view{image};
  M68kAnalysisConfig config{};
  config.domains.address = address;
  config.accept_width_domains = accept_width;
  return analyze_m68k_finite_values(view, {entry}, config);
}

std::optional<std::vector<std::uint32_t>> values_at(const M68kFiniteAnalysisResult &result, std::uint32_t pc, unsigned reg) {
  return m68k_query_address_register(result, pc, reg).values();
}

const M68kAddressSiteReport *site_at(const M68kFiniteAnalysisResult &result, std::uint32_t pc) {
  const auto found = result.address_sites.find(pc);
  return found == result.address_sites.end() ? nullptr : &found->second;
}

bool unknown_site(const M68kFiniteAnalysisResult &result, std::uint32_t pc, UnknownReason reason, Sub sub) {
  const auto *site = site_at(result, pc);
  return site != nullptr && !site->resolved && site->targets.empty() && site->reason == reason && site->sub == sub &&
         result.unresolved_computed.contains(pc) && !result.solution.computed_targets.contains(pc);
}

using Targets = std::vector<std::uint32_t>;

// ---------------------------------------------------------------------------------------------------------------
// Lattice.

void lattice() {
  const M68kRegion ram{M68kRegionKind::work_ram, 0U, work_ram_base, 0x200000U};
  std::vector<std::uint32_t> many;
  for (std::uint32_t i = 0; i < 65U; ++i) many.push_back(0x100U + 8U * i);
  const auto strided = M68kOffsetSet::of(many);
  expect(strided.is_strided() && strided.lo() == 0x100U && strided.stride() == 8U && strided.hi() == 0x100U + 8U * 64U,
         "offset set: more than 64 exact offsets become the tight strided hull");
  expect(!M68kOffsetSet::of({4U, 12U}).is_strided() && M68kOffsetSet::strided(0U, 4U, 8U).exact() == std::vector<std::uint32_t>{0U, 4U, 8U},
         "offset set: a small strided form normalizes to exact");
  const auto with_odd = join(strided, M68kOffsetSet::of({0x104U}), ram.limit());
  expect(with_odd.is_strided() && with_odd.stride() == 4U && leq(strided, with_odd) && !leq(with_odd, strided),
         "offset set: join keeps the congruence gcd");
  // Repeated strict growth widens to the region extent with the congruence kept, never to certainty.
  auto grown = strided;
  for (std::uint32_t i = 0; i < m68k_strided_growth_bound + 1U; ++i)
    grown = join(grown, M68kOffsetSet::of({grown.hi() + 8U}), ram.limit());
  expect(grown.is_strided() && grown.stride() == 8U && grown.lo() == 0U && grown.hi() == ram.limit() - ram.limit() % 8U,
         "offset set: growth past the bound widens only to the region extent");
  expect(!grown.shifted({8}, ram.limit()).has_value(), "offset set: stepping past the extent is never clamped");
  expect(strided.shifted({-0x100}, ram.limit())->lo() == 0U && !strided.shifted({-0x101}, ram.limit()).has_value(),
         "offset set: stepping below the region start leaves the region");
  expect(strided.restricted(0x108U, 0x118U).exact() == std::vector<std::uint32_t>{0x108U, 0x110U, 0x118U},
         "offset set: interval restriction keeps the congruence");

  std::vector<std::pair<M68kRegion, M68kOffsetSet>> nine;
  for (std::uint32_t i = 0; i < 9U; ++i) nine.emplace_back(M68kRegion{M68kRegionKind::image, i, 0U, 0x100U}, M68kOffsetSet::of({0U}));
  const auto over = M68kPointsTo::of(nine);
  expect(over.is_unknown() && over.reason == UnknownReason::set_bound && over.sub == Sub::set_bound,
         "points-to: more than 8 regions is Unknown(set_bound)");
  const auto a = M68kPointsTo::unknown(UnknownReason::unsupported_transfer, Sub::region_exit);
  const auto b = M68kPointsTo::unknown(UnknownReason::unknown_input);
  expect(join(a, b) == b && join(b, a) == b && leq(a, b) && !leq(b, a), "points-to: the smaller (reason, sub) absorbs");
  const auto p = M68kPointsTo::of({{ram, M68kOffsetSet::of({0x10U})}});
  expect(leq(p, a) && !leq(a, p) && join(p, M68kPointsTo::bottom()) == p, "points-to: known below Unknown, bottom neutral");
  expect(m68k_points_to_add(p, {-0x11}).sub == Sub::region_exit, "points-to: arithmetic leaving the region is region_exit");
}

// ---------------------------------------------------------------------------------------------------------------
// Fixtures.

// An object-slot loop: LEA base; loop: NOP; LEA $40(A0),A0; CMPA.L #end,A0; BCS loop. The stepped pointer keeps one work-RAM
// region and its congruence (100 slots: beyond the exact bound, within the growth bound).
void object_slot_loop() {
  Image image;
  image.words(0x200U, {0x41F9U, 0x00FFU, 0x0000U});          // LEA $00FF0000,A0
  image.words(0x206U, {0x4E71U});                            // loop: NOP
  image.words(0x208U, {0x41E8U, 0x0040U});                   // LEA $40(A0),A0
  image.words(0x20CU, {0xB1FCU, 0x00FFU, 0x1900U});          // CMPA.L #$00FF1900,A0
  image.words(0x212U, {0x65F2U});                            // BCS.S loop
  image.words(0x214U, {0x4E71U, 0x60FEU});                   // NOP; BRA *
  const auto result = run(image, true);
  expect(result.complete, "object loop: complete");
  const auto head = m68k_query_address_register(result, 0x206U, 0);
  const bool one_region = head.is_known() && head.pairs.size() == 1U && head.pairs[0].first.kind == M68kRegionKind::work_ram;
  expect(one_region && head.pairs[0].second.is_strided() && head.pairs[0].second.stride() == 0x40U &&
             head.pairs[0].second.lo() == 0xFF0000U - work_ram_base && head.pairs[0].second.hi() == 0xFF18C0U - work_ram_base,
         "object loop: one region, stride $40, exactly the 100 slot offsets at the head: " + head.describe());
  expect(values_at(result, 0x214U, 0) == std::vector<std::uint32_t>{0xFF1900U}, "object loop: the exit edge is the exact end");

  // The same walk without a pointer guard (DBF on a counter) cannot be bounded by a non-relational domain: once the growth bound
  // is exhausted the set widens to the region extent and the next step leaves the region (region_exit), never a clamp.
  Image unguarded;
  unguarded.words(0x200U, {0x41F9U, 0x00FFU, 0x0000U});  // LEA $00FF0000,A0
  unguarded.words(0x206U, {0x7209U});                    // MOVEQ #9,D1
  unguarded.words(0x208U, {0x20C0U});                    // loop: MOVE.L D0,(A0)+
  unguarded.words(0x20AU, {0x51C9U, 0xFFFCU});           // DBF D1,loop
  unguarded.words(0x20EU, {0x4E71U, 0x60FEU});
  const auto walk = run(unguarded, true);
  const auto at = m68k_query_address_register(walk, 0x208U, 0);
  expect(walk.complete && at.is_unknown() && at.reason == UnknownReason::unsupported_transfer && at.sub == Sub::region_exit,
         "region overflow: an unguarded stepping walk ends Unknown(region_exit): " + at.describe());
}

void table_fixture(Image &image) {
  image.put32(0x280U, 0x300U);
  image.put32(0x284U, 0x340U);
  image.words(0x300U, {0x4E71U, 0x4E75U});  // NOP; RTS
  image.words(0x340U, {0x4E71U, 0x4E75U});
}

// MOVE.B ($F100).W,D0; ANDI.W #4,D0; MOVEA.L (table,PC,D0.W),A1; JSR (A1): a code pointer loaded from an immutable table.
void code_pointer_table() {
  Image image;
  table_fixture(image);
  image.words(0x200U, {0x1038U, 0xF100U, 0x0240U, 0x0004U, 0x227BU, 0x0076U, 0x4E91U, 0x60FEU});
  const auto result = run(image, true);
  const auto *site = site_at(result, 0x20CU);
  expect(result.complete && site != nullptr && site->resolved && site->targets == Targets{0x300U, 0x340U} &&
             site->family == M68kDynamicControlFamily::call_address_indirect,
         "code pointer: JSR (A1) resolves to the exact table entries");
  expect(result.reached.contains(0x300U) && result.reached.contains(0x342U), "code pointer: both targets are discovered");
  expect(values_at(result, 0x20CU, 1) == std::vector<std::uint32_t>{0x300U, 0x340U}, "code pointer: A1 query");

  // Baseline: the domain is inert, every address register stays Unknown and the site is unresolved exactly as before.
  const auto baseline = run(image, false);
  expect(baseline.complete && !baseline.address_domain && baseline.address_sites.empty() &&
             baseline.unresolved_computed.at(0x20CU) == UnknownReason::unsupported_transfer && !baseline.reached.contains(0x300U),
         "baseline: the (An) site stays unresolved with the domain off");
  expect(format_m68k_finite_analysis(baseline).find(" a1=") == std::string::npos, "baseline: no address state is reported");
  bool inert = true;
  for (const auto &[point, state] : baseline.solution.in_states)
    for (const auto &pointer : state.address) inert = inert && pointer == M68kPointsTo::unknown(UnknownReason::unknown_input);
  expect(inert, "baseline: every address register is Unknown(unknown_input) at every point");
  expect(format_m68k_finite_analysis(result) == format_m68k_finite_analysis(run(image, true)), "determinism: repeated output");
}

// LEA table(PC),A0; MOVEA.L 4(A0),A1; JSR 2(A1); then JMP 2(A2,D0.W) into a branch block whose paths reach JSR 0(A2,D0.W)
// (the index and base survive the computed edge and the branches; LEA keeps the data registers).
void displacement_and_index_forms() {
  Image image;
  table_fixture(image);
  image.words(0x200U, {0x41FAU, 0x007EU});  // LEA $280(PC),A0
  image.words(0x204U, {0x2268U, 0x0004U});  // MOVEA.L 4(A0),A1
  image.words(0x208U, {0x4EA9U, 0x0002U});  // JSR 2(A1)
  image.words(0x20CU, {0x1038U, 0xF100U, 0x0240U, 0x0004U});  // MOVE.B ($F100).W,D0; ANDI.W #4,D0
  image.words(0x214U, {0x45FAU, 0x016AU});  // LEA $380(PC),A2
  image.words(0x218U, {0x4EF2U, 0x0002U});  // JMP 2(A2,D0.W) -> $382 / $386
  image.words(0x380U, {0x4E71U, 0x600CU, 0x4E75U, 0x6008U});  // NOP; BRA.S $390; RTS; BRA.S $390
  image.words(0x390U, {0x4EB2U, 0x0000U, 0x60FEU});            // JSR 0(A2,D0.W) -> $380 / $384
  const auto result = run(image, true);
  const auto *d16 = site_at(result, 0x208U);
  expect(d16 != nullptr && d16->resolved && d16->targets == Targets{0x342U} &&
             d16->family == M68kDynamicControlFamily::call_address_disp16,
         "d16(An): JSR 2(A1) resolves to the loaded entry plus the displacement");
  const auto *jsr_index = site_at(result, 0x390U);
  expect(jsr_index != nullptr && jsr_index->resolved && jsr_index->targets == Targets{0x380U, 0x384U} &&
             jsr_index->family == M68kDynamicControlFamily::call_address_index,
         "(d8,An,Xn): JSR resolves over the exact index set");
  const auto *jmp_index = site_at(result, 0x218U);
  expect(jmp_index != nullptr && jmp_index->resolved && jmp_index->targets == Targets{0x382U, 0x386U} &&
             jmp_index->family == M68kDynamicControlFamily::jump_address_index,
         "(d8,An,Xn): JMP resolves over the exact index set");
}

// MOVEA.L ($F000).W,A1 (mutable work RAM); JSR (A1); JSR (A2) (never written).
void unknown_base() {
  Image image;
  image.words(0x200U, {0x2278U, 0xF000U, 0x4E91U, 0x4E92U, 0x60FEU});
  const auto result = run(image, true);
  expect(unknown_site(result, 0x204U, UnknownReason::non_immutable_read, Sub::base_unknown),
         "unknown base: a pointer loaded from mutable memory is Unknown(non_immutable_read/base_unknown)");
  expect(unknown_site(result, 0x206U, UnknownReason::unknown_input, Sub::base_unknown),
         "unknown base: an unwritten register is Unknown(unknown_input/base_unknown)");
}

// (An)+ / -(An) step the same base they access through, by the access size (byte through A7 keeps word alignment).
void auto_increment() {
  Image image;
  table_fixture(image);
  image.words(0x200U, {0x41FAU, 0x007EU});          // LEA $280(PC),A0
  image.words(0x204U, {0x2258U, 0x2458U});          // MOVEA.L (A0)+,A1; MOVEA.L (A0)+,A2
  image.words(0x208U, {0x47F9U, 0x00FFU, 0x0000U}); // LEA $00FF0000,A3
  image.words(0x20EU, {0x36C0U, 0x26C0U, 0x4223U}); // MOVE.W D0,(A3)+; MOVE.L D0,(A3)+; CLR.B -(A3)
  image.words(0x214U, {0x4FF9U, 0x00FFU, 0xFE00U}); // LEA $00FFFE00,A7
  image.words(0x21AU, {0x1F00U});                   // MOVE.B D0,-(A7)
  image.words(0x21CU, {0xD0FCU, 0xFFFCU});          // ADDA.W #-4,A0
  image.words(0x220U, {0xC34BU});                   // EXG A1,A3
  image.words(0x222U, {0x4E92U, 0x60FEU});          // JSR (A2)
  const auto result = run(image, true);
  expect(values_at(result, 0x222U, 0) == std::vector<std::uint32_t>{0x284U}, "auto-increment: two long loads then ADDA.W #-4");
  expect(values_at(result, 0x222U, 3) == std::vector<std::uint32_t>{0x300U} &&
             values_at(result, 0x222U, 1) == std::vector<std::uint32_t>{0xFF0005U},
         "auto-increment: the store cursor steps 2 + 4 - 1 and EXG swaps it");
  expect(values_at(result, 0x222U, 7) == std::vector<std::uint32_t>{0xFFFDFEU}, "auto-increment: a byte push through A7 moves 2");
  const auto *site = site_at(result, 0x222U);
  expect(site != nullptr && site->resolved && site->targets == Targets{0x340U}, "auto-increment: the second entry resolves");
  // The call moves A7 (the return address push): its value at the target is not tracked.
  expect(m68k_query_address_register(result, 0x340U, 7).is_unknown(), "auto-increment: JSR leaves A7 Unknown at the target");
}

// LEA $FFC(PC),A0; ADDQ.L #8,A0 leaves the image region; LEA $FFC(PC),A1; ADDQ.L #4,A1 is one past the end (admitted).
void region_overflow() {
  Image image;
  image.words(0x200U, {0x41FAU, 0x0DFAU, 0x5088U});  // LEA $FFC(PC),A0; ADDQ.L #8,A0
  image.words(0x206U, {0x43FAU, 0x0DF4U, 0x5889U});  // LEA $FFC(PC),A1; ADDQ.L #4,A1
  image.words(0x20CU, {0x4E90U, 0x60FEU});           // JSR (A0)
  const auto result = run(image, true);
  const auto a0 = m68k_query_address_register(result, 0x20CU, 0);
  expect(a0.is_unknown() && a0.sub == Sub::region_exit, "region overflow: arithmetic past the image extent is region_exit");
  expect(values_at(result, 0x20CU, 1) == std::vector<std::uint32_t>{image_size}, "region overflow: one past the end is kept");
  expect(unknown_site(result, 0x20CU, UnknownReason::unsupported_transfer, Sub::region_exit),
         "region overflow: the site reports region_exit");
}

// 128 code pointers (an explicit 128-value index) give a strided A1: the site stays Unknown(set_bound), never enumerated.
void strided_only() {
  Image image;
  image.words(0x200U, {0x3038U, 0xF100U, 0x0240U, 0x01FCU});  // MOVE.W ($F100).W,D0; ANDI.W #$1FC,D0
  image.words(0x208U, {0x41FAU, 0x01F6U});                    // LEA $400(PC),A0
  image.words(0x20CU, {0x2270U, 0x0000U, 0x4E91U, 0x60FEU});  // MOVEA.L 0(A0,D0.W),A1; JSR (A1)
  for (std::uint32_t i = 0; i < 128U; ++i) {
    image.put32(0x400U + 4U * i, 0x800U + 4U * i);
    image.words(0x800U + 4U * i, {0x4E71U, 0x4E75U});
  }
  const auto result = run(image, true);
  const auto a1 = m68k_query_address_register(result, 0x210U, 1);
  expect(a1.is_known() && !a1.is_exact(), "strided only: A1 is a known strided image pointer: " + a1.describe());
  expect(unknown_site(result, 0x210U, UnknownReason::set_bound, Sub::set_bound), "strided only: the site is Unknown(set_bound)");
  bool none = true;
  for (std::uint32_t i = 0; i < 128U; ++i) none = none && !result.reached.contains(0x800U + 4U * i);
  expect(none, "strided only: no strided member is ever taken as a target");
}

// LEA $300(PC),A0; MOVEQ #0,D0; MOVE.B ($F100).W,D0; JMP 0(A0,D0.W): a width-only index is not an explicit bound under the strict policy.
void width_only_index() {
  Image image;
  image.words(0x200U, {0x41FAU, 0x00FEU, 0x7000U, 0x1038U, 0xF100U, 0x4EF0U, 0x0000U});  // ... MOVEQ #0,D0 first
  const auto strict = run(image, true);
  expect(unknown_site(strict, 0x20AU, UnknownReason::unsupported_transfer, Sub::width_only), "width only: strict policy rejects");
  const auto measured = run(image, true, true);
  const auto *site = site_at(measured, 0x20AU);
  expect(site != nullptr && site->resolved && site->targets.size() == 128U && site->odd_targets_excluded == 128U,
         "width only: the measurement variant admits the 128 even targets and excludes the odd ones");
}

// A table with one entry outside the image: the whole site fails closed (no partial target set).
void target_outside_image() {
  Image image;
  table_fixture(image);
  image.put32(0x284U, 0x00FF0000U);
  image.words(0x200U, {0x41FAU, 0x007EU, 0x1038U, 0xF100U, 0x0240U, 0x0004U});  // LEA $280(PC),A0; MOVE.B; ANDI.W #4,D0
  image.words(0x20CU, {0x2270U, 0x0000U, 0x4E91U, 0x60FEU});                    // MOVEA.L 0(A0,D0.W),A1; JSR (A1)
  const auto result = run(image, true);
  expect(unknown_site(result, 0x210U, UnknownReason::non_immutable_read, Sub::target_outside_image),
         "outside image: one unmapped target fails the whole site");
  expect(!result.reached.contains(0x300U), "outside image: the in-image entry is not taken either");
}

}  // namespace

int main() {
  lattice();
  object_slot_loop();
  code_pointer_table();
  displacement_and_index_forms();
  unknown_base();
  auto_increment();
  region_overflow();
  strided_only();
  width_only_index();
  target_outside_image();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cout << "analysis_m68k_value_test: all checks passed\n";
  return EXIT_SUCCESS;
}
