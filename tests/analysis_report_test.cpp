// SEG-030-T002 (ADR 0079): the report-only Genesis M68K analysis driver over the executable-image view. Project-authored
// synthetic MC68000 fixtures only (real encodings decoded by the unchanged decoder/lifter); no commercial input.
//
// 1. An ADR 0049 alias image executes at its work-RAM base, and the instruction bytes come from its cartridge source.
// 2. A read through the alias execution address is mutable work RAM, never immutable: the same PC-indexed dispatch resolves in
//    the cartridge and fails closed (non_immutable_read) when it executes from the alias.
// 3. An instruction crossing the alias end is rejected (it is not cartridge-executable at that address).
// 4. `genesis_reachability_roots` is the challenger's root owner, and the baseline driver reproduces the challenger's D, unresolved
//    sites and PC-index outcomes on the fixture.
// 5. With an output directory argument, writes the fixture ROM, its digest, a synthetic coverage directory and the library's
//    private report for the driver/compare-tool test (analysis_report_tests.cmake in script mode).

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

#include "segarecomp/genesis_analysis_report/report.hpp"
#include "segarecomp/sha256.hpp"

namespace {

using namespace segarecomp;

int failures = 0;
void expect(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

constexpr std::uint32_t entry = 0x200U;
constexpr unsigned HI = 2U;
constexpr std::uint32_t alias_a = 0xFF0000U, alias_a_source = 0x400U, alias_a_length = 0x80U;
constexpr std::uint32_t alias_b = 0xFF0100U, alias_b_source = 0x500U, alias_b_length = 0x4U;

struct Image {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x800U, 0U);
  Image() {
    put32(0x0U, 0x00FFFE00U);
    put32(0x4U, entry);
  }
  void put32(std::uint32_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4U; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (24U - 8U * i));
  }
  void words(std::uint32_t at, std::initializer_list<std::uint16_t> values) {
    for (const auto value : values) {
      bytes[at] = static_cast<std::uint8_t>(value >> 8U);
      bytes[at + 1U] = static_cast<std::uint8_t>(value);
      at += 2U;
    }
  }
};

struct Asm {
  Image &image;
  std::uint32_t pc;
  Asm &w(std::uint32_t value) {
    image.words(pc, {static_cast<std::uint16_t>(value)});
    pc += 2U;
    return *this;
  }
  Asm &moveq(unsigned d, std::uint8_t n) { return w(0x7000U | d << 9U | n); }
  Asm &move_b_ram(unsigned d, std::uint16_t address) { return w(0x1038U | d << 9U).w(address); }
  Asm &cmpi_b(unsigned d, std::uint8_t imm) { return w(0x0C00U | d).w(imm); }
  Asm &bcc_s(unsigned condition, std::uint32_t to) { return w(0x6000U | condition << 8U | ((to - pc - 2U) & 0xFFU)); }
  Asm &add_w_self(unsigned d) { return w(0xD040U | d << 9U | d); }
  Asm &index_ext(unsigned index, std::uint32_t table) { return w(index << 12U | ((table - pc) & 0xFFU)); }
  Asm &move_w_pcidx(unsigned index, unsigned destination, std::uint32_t table) {
    return w(0x303BU | destination << 9U).index_ext(index, table);
  }
  Asm &jmp_pcidx(unsigned index, std::uint32_t table) { return w(0x4EFBU).index_ext(index, table); }
  Asm &jmp_abs(std::uint32_t to) { return w(0x4EF9U).w(to >> 16U).w(to); }
  Asm &nop() { return w(0x4E71U); }
  Asm &bra_self() { return w(0x60FEU); }
  Asm &rte() { return w(0x4E73U); }
};

// A guarded word-offset table dispatch at `at` (table at `table`, out-of-range path at `out`); returns the JMP's PC.
std::uint32_t dispatch(Image &image, std::uint32_t at, std::uint32_t out, std::uint32_t table) {
  Asm a{image, at};
  a.moveq(0, 0).move_b_ram(0, 0xF100U).cmpi_b(0, 1U).bcc_s(HI, out).add_w_self(0).move_w_pcidx(0, 0, table);
  const auto jmp = a.pc;
  a.jmp_pcidx(0, table);
  image.words(table, {0x0010U, 0x0020U});  // targets table+0x10, table+0x20
  return jmp;
}

struct Fixture {
  Image image;
  std::uint32_t rom_jmp{}, alias_jmp{};
};

// Reset code in the cartridge dispatches through an immutable table, then every path jumps to alias A (a verbatim copy of the
// same dispatch shape, executing from work RAM), whose paths jump to alias B, whose second instruction crosses the alias end.
Fixture build() {
  Fixture f;
  f.rom_jmp = dispatch(f.image, 0x200U, 0x260U, 0x230U);
  for (const auto at : {0x240U, 0x250U, 0x260U}) Asm{f.image, at}.jmp_abs(alias_a);
  const auto source_jmp = dispatch(f.image, alias_a_source, alias_a_source + 0x60U, alias_a_source + 0x30U);
  f.alias_jmp = alias_a + (source_jmp - alias_a_source);
  for (const auto at : {0x40U, 0x50U, 0x60U}) Asm{f.image, alias_a_source + at}.jmp_abs(alias_b);
  Asm{f.image, alias_b_source}.nop().jmp_abs(0x600U);  // the 6-byte JMP starts 2 bytes before the 4-byte alias end
  Asm{f.image, 0x600U}.bra_self();
  Asm{f.image, 0x620U}.rte();
  f.image.put32(30U * 4U, 0x600U);  // IRQ6
  f.image.put32(32U * 4U, 0x600U);  // TRAP #0 (shares the handler)
  f.image.put32(4U * 4U, 0x620U);   // illegal instruction
  return f;
}

std::optional<FrontendProgram> program_of(const Image &image) {
  auto program = make_genesis_bridge_startup_program(image.bytes, 0U, entry, std::nullopt);
  if (!program || !apply_genesis_immutable_rom_aot(*program) ||
      !apply_genesis_immutable_copy_alias(*program, alias_a, alias_a_source, alias_a_length) ||
      !apply_genesis_immutable_copy_alias(*program, alias_b, alias_b_source, alias_b_length))
    return std::nullopt;
  return program;
}

void view_rules(const FrontendProgram &program) {
  const auto view = GenesisM68kAnalysisImage::create(program);
  expect(view.has_value(), "view: the executable-image set validates");
  if (!view) return;
  std::size_t immutable = 0U, alias = 0U;
  for (const auto &image : view->images().set.images)
    (image.provenance.authority == ImageAuthority::immutable_input ? immutable : alias) += 1U;
  expect(immutable == 1U && alias == 2U, "view: one cartridge image plus two static_proof alias images");

  const auto in_rom = view->decode(alias_a_source);
  const auto in_alias = view->decode(alias_a);
  expect(in_rom && in_alias && in_rom->length == in_alias->length && in_rom->operation.kind == in_alias->operation.kind,
         "alias executes: the alias base decodes the cartridge source instruction");
  expect(view->mapped(alias_a) && view->mapped(alias_a + 0x10U), "alias executes: alias PCs are mapped");
  expect(!view->mapped(alias_a + alias_a_length) && !view->decode(alias_a + alias_a_length),
         "alias executes: the first byte past the alias is not executable");

  expect(view->immutable_read(alias_a_source + 0x30U, 2U) == 0x0010U, "immutable: cartridge table entry reads");
  expect(!view->immutable_read(alias_a + 0x30U, 2U), "alias read not immutable: the alias execution address is work RAM");
  expect(!view->immutable_read(0xFFFFFFU, 2U) && !view->immutable_read(0x7FFU, 2U), "immutable: reads past an image end fail closed");
  expect(!view->immutable_read(0x200U, 5U), "immutable: oversize read rejected");

  // SEG-030-T003: region extents (ADR 0079 decision 4).
  const auto cartridge = view->region_of(0x200U);
  expect(cartridge && cartridge->kind == M68kRegionKind::image && cartridge->base == 0U && cartridge->size == 0x800U,
         "regions: a cartridge address is in its immutable image extent");
  const auto ram = view->region_of(alias_a);
  expect(ram && ram->kind == M68kRegionKind::mutable_ram && ram->base == 0xE00000U && ram->size == 0x200000U,
         "regions: an alias execution address is work RAM");
  const auto io = view->region_of(0xC00004U);
  expect(io && io->kind == M68kRegionKind::io_device, "regions: the VDP port window is the I/O/device region");
  expect(!view->region_of(0x900U).has_value(), "regions: an address past the cartridge bytes has no region");

  expect(view->decode(alias_b_source + 2U).has_value(), "crossing: the instruction decodes at its cartridge address");
  expect(view->mapped(alias_b + 2U) && !view->decode(alias_b + 2U) &&
             view->status(alias_b + 2U) == GenesisM68kAnalysisImage::Status::rejected,
         "crossing: an instruction crossing the alias end is rejected");
  expect(view->status(0x201U) == GenesisM68kAnalysisImage::Status::odd, "status: odd");
  expect(view->status(0x900U) == GenesisM68kAnalysisImage::Status::unmapped, "status: unmapped");
}

void baseline_against_challenger(const Fixture &f, const FrontendProgram &program) {
  const auto roots = genesis_reachability_roots(program);
  GenesisReachabilityChallengerConfig challenger_config{};
  challenger_config.pc_index_recovery = true;
  const auto challenger = run_genesis_reachability_challenger(program, challenger_config);
  expect(roots.roots == challenger.roots && roots.vector_roots == challenger.vector_roots, "roots helper = challenger roots");
  expect(roots.roots == std::vector<std::uint32_t>{0x200U, 0x600U, 0x620U} && roots.vector_roots == 3U,
         "roots helper: reset entry plus the installed delivered vectors (shared handler once, counted per vector)");

  const GenesisAnalysisReportConfig config{};
  const auto report = run_genesis_analysis_report(program, config);
  expect(report.images_valid && report.analysis.complete, "driver: complete");
  expect(report.discovered.contains(alias_a) && report.discovered.contains(f.alias_jmp) && report.discovered.contains(alias_b),
         "driver: alias code is discovered at its execution base");
  expect(!report.discovered.contains(alias_b + 2U) && report.rejected_decode_targets.contains(alias_b + 2U),
         "driver: the crossing instruction is a rejected decode target, not D");

  const auto rom_site = report.computed_sites.find(f.rom_jmp);
  expect(rom_site != report.computed_sites.end() && rom_site->second.resolved &&
             rom_site->second.family == GenesisAnalysisFamily::pc_index_explicit &&
             rom_site->second.targets == std::vector<std::uint32_t>{0x240U, 0x250U},
         "driver: the cartridge dispatch resolves through immutable table entries");
  const auto alias_site = report.computed_sites.find(f.alias_jmp);
  expect(alias_site != report.computed_sites.end() && !alias_site->second.resolved &&
             alias_site->second.reason == analysis::UnknownReason::non_immutable_read &&
             report.analysis.pc_index_sites.at(f.alias_jmp).outcome == M68kPcIndexOutcome::entry_outside_immutable_image,
         "alias read not immutable: the same dispatch from the alias fails closed (non_immutable_read)");
  expect(!report.discovered.contains(alias_a + 0x40U) && !report.discovered.contains(alias_a + 0x50U),
         "alias read not immutable: no target is taken from mutable table bytes");
  const auto rte_site = report.computed_sites.find(0x620U);
  expect(rte_site != report.computed_sites.end() && rte_site->second.family == GenesisAnalysisFamily::rte && !rte_site->second.resolved,
         "driver: RTE is a reported Unknown site");

  const auto comparison = compare_genesis_analysis_with_challenger(report, challenger);
  expect(comparison.core_only.empty() && comparison.challenger_only.empty() && comparison.both == challenger.discovered.size(),
         "baseline: D equals the challenger's D");
  expect(comparison.pc_index_differences.empty() && comparison.pc_index_core_only_sites == 0U &&
             comparison.pc_index_challenger_only_sites == 0U,
         "baseline: PC-index outcomes and target sets equal the challenger's");
  bool sites_equal = true;
  for (const auto difference : comparison.unresolved_site_differences) sites_equal = sites_equal && difference == 0U;
  expect(sites_equal, "baseline: unresolved sites per family equal the challenger's");
  expect(report.rejected_decode_targets == challenger.rejected_decode_targets && report.unmapped_targets == challenger.unmapped_targets &&
             report.odd_targets == challenger.odd_targets,
         "baseline: rejected/unmapped/odd targets equal the challenger's");

  const auto aggregate = format_genesis_analysis_report_aggregate(report, config);
  const auto again = format_genesis_analysis_report_aggregate(run_genesis_analysis_report(program, config), config);
  expect(aggregate == again, "determinism: repeated aggregate is identical");
  expect(aggregate.find("ff0") == std::string::npos && aggregate.find("\"200\"") == std::string::npos,
         "aggregate carries no address");

  // A lowered bound is never partial truth.
  GenesisAnalysisReportConfig bounded{};
  bounded.bounds.max_iterations = 3U;
  const auto exhausted = run_genesis_analysis_report(program, bounded);
  expect(!exhausted.analysis.complete && exhausted.discovered.empty() && exhausted.computed_sites.empty(),
         "bounds: an exhausted solve reports no partial D");
}

// SEG-030-T003: LEA $280(PC),A0; MOVEA.L (A0),A1; JSR (A1) through the Genesis view and the report families.
void address_domain_report() {
  Image image;
  image.words(0x200U, {0x41FAU, 0x007EU, 0x2250U, 0x4E91U, 0x60FEU});
  image.put32(0x280U, 0x300U);
  image.words(0x300U, {0x4E71U, 0x4E75U});
  auto program = make_genesis_bridge_startup_program(image.bytes, 0U, entry, std::nullopt);
  expect(program && apply_genesis_immutable_rom_aot(*program), "address fixture program");
  if (!program) return;
  constexpr std::uint32_t site_pc = 0x206U;
  const auto jsr_family = static_cast<std::size_t>(M68kDynamicControlFamily::call_address_indirect);

  const GenesisAnalysisReportConfig baseline{};
  const auto off = run_genesis_analysis_report(*program, baseline);
  const auto off_site = off.computed_sites.find(site_pc);
  expect(off_site != off.computed_sites.end() && off_site->second.family == GenesisAnalysisFamily::jsr_an &&
             !off_site->second.resolved && off_site->second.reason == analysis::UnknownReason::unsupported_transfer &&
             off_site->second.detail == GenesisAnalysisSubReason::none && !off.discovered.contains(0x300U) &&
             off.sites[jsr_family].contains(site_pc),
         "address off: the JSR (An) site is the baseline unresolved site");
  expect(format_genesis_analysis_report_aggregate(off, baseline).find("address_recovery") == std::string::npos,
         "address off: the baseline aggregate is unchanged (no address members)");

  GenesisAnalysisReportConfig config{};
  config.domains.address = true;
  const auto on = run_genesis_analysis_report(*program, config);
  const auto on_site = on.computed_sites.find(site_pc);
  expect(on.analysis.complete && on_site != on.computed_sites.end() && on_site->second.family == GenesisAnalysisFamily::jsr_an &&
             on_site->second.resolved && on_site->second.targets == std::vector<std::uint32_t>{0x300U},
         "address on: JSR (An) resolves from the immutable table entry");
  expect(on.discovered.contains(0x300U) && on.discovered.contains(0x302U) && !on.sites[jsr_family].contains(site_pc),
         "address on: the target is discovered and the site is no longer an unresolved dynamic site");
  const auto aggregate = format_genesis_analysis_report_aggregate(on, config);
  expect(aggregate.find("\"jsr_an\":{\"sites\":1,\"resolved\":1,") != std::string::npos &&
             aggregate.find("\"address_recovery\":{\"sites_encountered\":1,\"resolved\":1,") != std::string::npos,
         "address on: per-family and address-recovery aggregates: " + aggregate);
  expect(aggregate == format_genesis_analysis_report_aggregate(run_genesis_analysis_report(*program, config), config),
         "address on: deterministic aggregate");
  const auto private_output = format_genesis_analysis_report_private(on, aggregate);
  expect(private_output.find("\"000206\":{\"family\":\"jsr_an\",\"outcome\":\"resolved\"") != std::string::npos,
         "address on: the private computed_sites entry carries the resolved family");
}

void reject_invalid_images() {
  Image image;
  auto program = make_genesis_bridge_startup_program(image.bytes, 0U, entry, std::nullopt);
  expect(program.has_value(), "fixture program");
  if (!program) return;
  FrontendProgram::ImmutableCopyAlias bogus{0x00FF0000U, 0x00100000U, 0x10U};  // source outside every cartridge claim
  program->immutable_copy_aliases.push_back(bogus);
  expect(!GenesisM68kAnalysisImage::create(*program).has_value(), "view: an unowned alias source fails closed");
  const auto report = run_genesis_analysis_report(*program, {});
  expect(!report.images_valid && report.discovered.empty(), "driver: an invalid image set yields no D");
}

void write_driver_inputs(const std::filesystem::path &dir, const Fixture &f, const FrontendProgram &program) {
  std::filesystem::create_directories(dir / "coverage");
  {
    std::ofstream rom{dir / "rom.bin", std::ios::binary};
    rom.write(reinterpret_cast<const char *>(f.image.bytes.data()), static_cast<std::streamsize>(f.image.bytes.size()));
  }
  std::ofstream{dir / "rom.sha256", std::ios::binary} << sha256_hex(f.image.bytes);
  const GenesisAnalysisReportConfig config{};
  const auto report = run_genesis_analysis_report(program, config);
  std::ofstream{dir / "library.json", std::ios::binary}
      << format_genesis_analysis_report_private(report, format_genesis_analysis_report_aggregate(report, config));
  std::ofstream{dir / "discovered.txt", std::ios::binary} << report.discovered.size();
  // Synthetic coverage: the reset dispatch path up to its JMP, then its first proven target (witness: the resolved site).
  std::vector<std::uint8_t> bitmap(std::size_t{1} << 20U, 0U);
  std::ofstream witnesses{dir / "coverage" / "witnesses.txt", std::ios::binary};
  std::uint32_t ordinal = 0U, previous = 0U;
  for (const auto pc : {0x200U, 0x202U, 0x206U, 0x20AU, 0x20CU, 0x20EU, f.rom_jmp, 0x240U}) {
    bitmap[(pc >> 1U) >> 3U] |= static_cast<std::uint8_t>(1U << ((pc >> 1U) & 7U));
    witnesses << ordinal << ' ' << std::hex << previous << ' ' << pc << std::dec << ' ' << (ordinal == 0U ? 0 : 1) << '\n';
    previous = pc;
    ++ordinal;
  }
  std::ofstream{dir / "coverage" / "coverage.bitmap", std::ios::binary}
      .write(reinterpret_cast<const char *>(bitmap.data()), static_cast<std::streamsize>(bitmap.size()));
}

}  // namespace

int main(int argc, char **argv) {
  const auto f = build();
  const auto program = program_of(f.image);
  expect(program.has_value(), "fixture program with two aliases");
  if (program) {
    view_rules(*program);
    baseline_against_challenger(f, *program);
    if (argc > 1) write_driver_inputs(argv[1], f, *program);
  }
  address_domain_report();
  reject_invalid_images();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cout << "analysis_report_test: all checks passed\n";
  return EXIT_SUCCESS;
}
