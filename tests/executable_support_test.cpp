// SEG-024-T001: executable-support experiment fixtures (project-authored, ROM-independent).
//
// Generic fixed-point fixtures use abstract identities; Genesis fixtures use real MC68000 encodings through the
// unchanged decoder/lifter/immutable-ROM AOT admission, so exact synthetic addresses may be asserted.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "segarecomp/cpu/m68k/control_support.hpp"
#include "segarecomp/cpu/m68k/decode.hpp"
#include "segarecomp/machine/genesis/executable_support.hpp"
#include "segarecomp/recompiler/executable_support.hpp"

namespace {

using namespace segarecomp;

int failures = 0;
void expect(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

bool live(const ExecutableSupportResult &result, ExecutableIdentity id) {
  return std::binary_search(result.live.begin(), result.live.end(), id);
}

SupportCandidate candidate(ExecutableIdentity id, std::vector<SupportEdge> successors = {}) {
  return {id, std::move(successors)};
}

std::vector<ExecutableIdentity> ids_of(const ExecutableSupportInput &input) {
  std::vector<ExecutableIdentity> ids;
  for (const auto &c : input.universe) ids.push_back(c.identity);
  return ids;
}

// ---------------------------------------------------------------------------------------------------------
// Generic fixtures
// ---------------------------------------------------------------------------------------------------------

// Fixture 1 (generic): a decodable island with no executable support is in U but not in L.
void fixture1_unreachable_island() {
  ExecutableSupportInput input{};
  input.universe = {candidate(0, {{SupportSourceKind::fallthrough, 2}}), candidate(2, {{SupportSourceKind::fallthrough, 4}}),
                    candidate(4), candidate(100, {{SupportSourceKind::fallthrough, 102}}), candidate(102)};
  input.roots = {0};
  const auto result = compute_executable_support(input);
  expect(result.sound, "F1: no ablation is a sound result");
  expect(result.live == std::vector<ExecutableIdentity>{0, 2, 4}, "F1: only root-supported code is live");
  expect(!live(result, 100) && !live(result, 102), "F1: island is in U but not in L");
}

// Fixture 2: exact indirect domain {A,B} keeps exactly A and B.
void fixture2_exact_indirect() {
  ExecutableSupportInput input{};
  input.universe = {candidate(0), candidate(100), candidate(200), candidate(300)};
  input.roots = {0};
  input.family_count = 1;
  input.domains = {{TargetDomainKind::exact_set, DomainEvidence::cpu_semantics, {100, 200}, {}}};
  input.sites = {{0, 0, 0}};
  const auto result = compute_executable_support(input);
  expect(result.live == std::vector<ExecutableIdentity>{0, 100, 200}, "F2: exact domain members live, others removable");
  expect(result.first_live_by_domain_kind[static_cast<std::size_t>(TargetDomainKind::exact_set)] == 2U,
         "F2: attribution names the exact domain");
}

// Fixture 3: an unconstrained indirect (AnyImmutableRom) forces L = U: the system fails conservatively.
void fixture3_unconstrained_indirect() {
  ExecutableSupportInput input{};
  input.universe = {candidate(0), candidate(10), candidate(20), candidate(30)};
  input.roots = {0};
  input.family_count = 1;
  input.domains = {{TargetDomainKind::any_candidate, DomainEvidence::none, {}, {}}};
  input.sites = {{0, 0, 0}};
  const auto result = compute_executable_support(input);
  expect(result.live == ids_of(input), "F3: unknown -> KEEP: L = U");
  // A site owned by a dead candidate activates nothing.
  input.sites = {{30, 0, 0}};
  expect(compute_executable_support(input).live == std::vector<ExecutableIdentity>{0},
         "F3: a dynamic site activates only when its owner is live");
}

// Fixture 4: bounded region refinement. Starting from D = U, a sound proof D = R retains only U ∩ R.
void fixture4_bounded_region() {
  ExecutableSupportInput input{};
  input.universe = {candidate(0), candidate(100), candidate(120), candidate(148), candidate(150), candidate(400)};
  input.roots = {0};
  input.family_count = 1;
  const auto universe = ids_of(input);
  const TargetDomain broad{TargetDomainKind::any_candidate, DomainEvidence::none, {}, {}};
  const TargetDomain region{TargetDomainKind::bounded_region, DomainEvidence::architectural_operand_width, {}, {{100, 150}}};
  const auto narrowed = narrow_target_domain(broad, region, universe);
  expect(narrowed.kind == TargetDomainKind::bounded_region, "F4: an evidenced subset narrowing is accepted");
  input.domains = {narrowed};
  input.sites = {{0, 0, 0}};
  const auto result = compute_executable_support(input);
  expect(result.live == std::vector<ExecutableIdentity>{0, 100, 120, 148}, "F4: only U ∩ R retained by the site");
  // A proposal that is not a subset of the previous domain is rejected (previous domain kept).
  const TargetDomain previous{TargetDomainKind::exact_set, DomainEvidence::cpu_semantics, {100, 120}, {}};
  const TargetDomain wider{TargetDomainKind::exact_set, DomainEvidence::cpu_semantics, {100, 400}, {}};
  expect(narrow_target_domain(previous, wider, universe).members == previous.members,
         "F4: new_domain must be a subset of previous_domain, else rejected");
}

// Fixture 6 (generic): a disconnected cycle is not self-justifying.
void fixture6_circular_support() {
  ExecutableSupportInput input{};
  input.universe = {candidate(0), candidate(50, {{SupportSourceKind::direct_branch, 60}}),
                    candidate(60, {{SupportSourceKind::direct_branch, 50}})};
  input.roots = {0};
  const auto result = compute_executable_support(input);
  expect(result.live == std::vector<ExecutableIdentity>{0}, "F6: A -> B -> A with no entry stays out of L");
}

// Fixture 8: anti-circularity. The narrowing "the site cannot target X because X is not live" is only
// self-consistent if X was already removed. It must be rejected and X kept.
void fixture8_anti_circularity() {
  ExecutableSupportInput input{};
  input.universe = {candidate(0), candidate(7), candidate(9)};
  input.roots = {0};
  input.family_count = 1;
  input.domains = {{TargetDomainKind::any_candidate, DomainEvidence::none, {}, {}}};
  input.sites = {{0, 0, 0}};
  const auto universe = ids_of(input);
  // Candidate 9 is dead in the fixed-flow-only closure; a proposal derived from that live-set result:
  ExecutableSupportOptions fixed_only{};
  fixed_only.ignore_domain_kind.fill(true);
  const auto fixed = compute_executable_support(input, fixed_only);
  expect(!fixed.sound && !live(fixed, 9), "F8: setup: the unsound fixed-only closure omits candidate 9");
  std::vector<ExecutableIdentity> assumed;
  for (const auto id : universe)
    if (live(fixed, id) || id == 7) assumed.push_back(id);  // "everything except what was removed"
  const TargetDomain circular{TargetDomainKind::exact_set, DomainEvidence::live_set_derived, assumed, {}};
  // Had it been accepted, the result would be self-consistent and would drop 9 -- which is the trap.
  ExecutableSupportInput trap = input;
  trap.domains = {circular};
  expect(!live(compute_executable_support(trap), 9), "F8: setup: the circular narrowing would be self-consistent");
  input.domains[0] = narrow_target_domain(input.domains[0], circular, universe);
  expect(input.domains[0].kind == TargetDomainKind::any_candidate, "F8: live-set-derived narrowing is rejected");
  expect(live(compute_executable_support(input), 9), "F8: candidate 9 is kept");
  // A proposal with no evidence at all is equally rejected.
  const TargetDomain unevidenced{TargetDomainKind::exact_set, DomainEvidence::none, {7}, {}};
  expect(narrow_target_domain(input.domains[0], unevidenced, universe).kind == TargetDomainKind::any_candidate,
         "F8: an unevidenced narrowing is rejected");
}

void generic_determinism_and_ablation_flags() {
  ExecutableSupportInput input{};
  input.universe = {candidate(0, {{SupportSourceKind::fallthrough, 2}}), candidate(2), candidate(4), candidate(6)};
  input.roots = {0};
  input.family_count = 2;
  input.domains = {{TargetDomainKind::exact_set, DomainEvidence::cpu_semantics, {4}, {}},
                   {TargetDomainKind::any_candidate, DomainEvidence::none, {}, {}}};
  input.sites = {{2, 0, 0}, {4, 1, 1}};
  auto reordered = input;
  std::reverse(reordered.sites.begin(), reordered.sites.end());
  const auto a = compute_executable_support(input);
  const auto b = compute_executable_support(reordered);
  expect(a.live == b.live && a.rounds == b.rounds && a.first_reason_counts == b.first_reason_counts,
         "determinism: site input order does not change L, rounds or reason counts");
  expect(executable_identity_digest(a.live) == executable_identity_digest(b.live), "determinism: identical digests");
  ExecutableSupportOptions ablate{};
  ablate.ignore_family = {false, true};
  const auto c = compute_executable_support(input, ablate);
  expect(!c.sound && c.live == std::vector<ExecutableIdentity>{0, 2, 4}, "ablation is flagged unsound");
  expect(c.live_sites_by_family[1] == 1U, "ablated sites are still counted as live sites");
}

// ---------------------------------------------------------------------------------------------------------
// MC68000 projection (consumes m68k_operation_effect through real decode/lift)
// ---------------------------------------------------------------------------------------------------------

M68kControlSupport support_of(std::vector<std::uint8_t> bytes, std::uint32_t address) {
  const DecodeSource source{CpuVariant::mc68000, {TargetAddressSpace::m68k_program, address}, MoveqImageOffset{0}};
  const auto decoded = decode_m68k_instruction(std::span<const std::uint8_t>(bytes), source, M68kDecodeProfile::general_startup);
  const auto *instruction = std::get_if<M68kDecodedInstruction>(&decoded);
  if (instruction == nullptr) {
    ++failures;
    std::cerr << "FAIL: fixture bytes do not decode\n";
    return {};
  }
  return m68k_control_support(lift_m68k_instruction(*instruction));
}

bool has(const M68kControlSupport &support, M68kControlSuccessorKind kind, std::uint32_t target) {
  return std::any_of(support.successors.begin(), support.successors.end(),
                     [&](const auto &s) { return s.kind == kind && s.target == target; });
}

void m68k_projection() {
  const auto bra = support_of({0x60, 0x06}, 0x1000);
  expect(bra.successors.size() == 1U && has(bra, M68kControlSuccessorKind::branch_target, 0x1008),
         "BRA.S: taken target only, no fallthrough");
  const auto bne = support_of({0x66, 0x06}, 0x1000);
  expect(has(bne, M68kControlSuccessorKind::conditional_target, 0x1008) &&
             has(bne, M68kControlSuccessorKind::conditional_fallthrough, 0x1002),
         "Bcc: both outcomes");
  const auto bsr = support_of({0x61, 0x00, 0x00, 0x10}, 0x1000);
  expect(has(bsr, M68kControlSuccessorKind::call_target, 0x1012) &&
             has(bsr, M68kControlSuccessorKind::call_continuation, 0x1004),
         "BSR.W: callee and continuation");
  const auto jsr_an = support_of({0x4E, 0x91}, 0x1000);
  expect(jsr_an.dynamic == M68kDynamicControlFamily::call_address_indirect &&
             has(jsr_an, M68kControlSuccessorKind::call_continuation, 0x1002) && !jsr_an.architectural_target_interval,
         "JSR (A1): dynamic, continuation, no architectural bound");
  expect(support_of({0x4E, 0x75}, 0x1000).dynamic == M68kDynamicControlFamily::return_from_subroutine, "RTS family");
  expect(support_of({0x4E, 0x73}, 0x1000).dynamic == M68kDynamicControlFamily::return_from_exception, "RTE family");
  const auto trap = support_of({0x4E, 0x41}, 0x1000);
  expect(has(trap, M68kControlSuccessorKind::exception_continuation, 0x1002), "TRAP #1: stacked next PC is an exception continuation");
  expect(support_of({0x4A, 0xFC}, 0x1000).successors.empty(), "ILLEGAL: no fixed successor (vector root owns the handler)");
  const auto pea = support_of({0x48, 0x78, 0x20, 0x00}, 0x1000);
  expect(has(pea, M68kControlSuccessorKind::pushed_code_address, 0x2000) &&
             has(pea, M68kControlSuccessorKind::fallthrough, 0x1004),
         "PEA abs.W: pushed code address + fallthrough");
  // JMP (4,PC,D0.W) at 0x1000: base = 0x1002 + 4, target in [base-32768, base+32768).
  const auto jmp_pc = support_of({0x4E, 0xFB, 0x00, 0x04}, 0x1000);
  expect(jmp_pc.dynamic == M68kDynamicControlFamily::jump_pc_index_word && jmp_pc.architectural_target_interval &&
             jmp_pc.architectural_target_interval->begin == 0x1006 - 32768 &&
             jmp_pc.architectural_target_interval->end == 0x1006 + 32768,
         "JMP (d8,PC,D0.W): operand-width interval");
  const auto jmp_pc_long = support_of({0x4E, 0xFB, 0x08, 0x04}, 0x1000);
  expect(jmp_pc_long.dynamic == M68kDynamicControlFamily::jump_pc_index_long && !jmp_pc_long.architectural_target_interval,
         "JMP (d8,PC,D0.L): no operand-width bound");
}

// ---------------------------------------------------------------------------------------------------------
// Genesis fixtures (vector table at 0, reset PC 0x200)
// ---------------------------------------------------------------------------------------------------------

std::vector<std::uint8_t> vector_image(std::size_t size) {
  std::vector<std::uint8_t> image(size, 0U);
  image[0] = 0x00; image[1] = 0xFF; image[2] = 0x01; image[3] = 0x00;  // SSP 0x00FF0100
  image[4] = 0x00; image[5] = 0x00; image[6] = 0x02; image[7] = 0x00;  // reset PC 0x200
  return image;
}

void put(std::vector<std::uint8_t> &image, std::size_t at, std::initializer_list<std::uint8_t> bytes) {
  for (const auto byte : bytes) image[at++] = byte;
}

FrontendProgram genesis_program(std::vector<std::uint8_t> image, const char *id) {
  FrontendProgram program{};
  program.profile = M68kFrontendProfile::general_startup;
  const auto size = image.size();
  program.image = {id, std::move(image), size};
  program.mapping_claims = {{"raw_cartridge_rom", {{}, 0U}, {{}, static_cast<std::uint32_t>(size)}, {0U}, {size}}};
  program.startup_ingress = M68kStartupIngress{{{}, 0x200U}, 0x00FF0100U};
  return program;
}

const FrontendAnalysis *accepted_analysis(const FrontendResult &result) {
  if (const auto *partial = std::get_if<FrontendPartialProgram>(&result)) return &partial->accepted_prefix;
  return std::get_if<FrontendAnalysis>(&result);
}

bool in_universe(const GenesisExecutableSupportModel &model, ExecutableIdentity id) {
  return std::any_of(model.input.universe.begin(), model.input.universe.end(),
                     [&](const auto &c) { return c.identity == id; });
}

// 0x200 BSR.W 0x20C            ; call + continuation 0x204
// 0x204 MOVE.L #$4E714E75,D0   ; overlapping decodes: 0x206 NOP, 0x208 RTS
// 0x20A BRA.S self
// Variant: vector 2 (bus error) = 0x206 gives the overlapping start architectural root support. (A direct
// branch into 0x206 would instead make Gen-2 CFG discovery reject the whole build: mid_instruction_direct_target.)
// 0x20C RTS                    ; returns through the Gen-2 continuation authority, never AnyImmutableRom
// 0x20E NOP / 0x210 NOP / 0x212 BRA.S 0x20E   ; unsupported island and disconnected cycle
std::vector<std::uint8_t> overlap_image(bool root_into_overlap) {
  auto image = vector_image(0x220);
  if (root_into_overlap) put(image, 0x08, {0x00, 0x00, 0x02, 0x06});
  put(image, 0x200, {0x61, 0x00, 0x00, 0x0A});
  put(image, 0x204, {0x20, 0x3C, 0x4E, 0x71, 0x4E, 0x75});
  put(image, 0x20A, {0x60, 0xFE});
  put(image, 0x20C, {0x4E, 0x75});
  put(image, 0x20E, {0x4E, 0x71, 0x4E, 0x71, 0x60, 0xFA});
  for (std::size_t at = 0x214; at < 0x220; at += 2) put(image, at, {0x4A, 0xFC});  // ILLEGAL filler
  return image;
}

void genesis_overlap_island_cycle_continuation() {
  for (const bool root_into_overlap : {false, true}) {
    auto program = genesis_program(overlap_image(root_into_overlap), "synthetic/SEG-024-T001/overlap-island-cycle");
    expect(apply_genesis_immutable_rom_aot(program), "overlap fixture enumerates immutable ROM");
    const auto result = analyze_m68k_frontend(program);
    const auto *analysis = accepted_analysis(result);
    if (analysis == nullptr) std::cerr << format_m68k_frontend_result(result) << "\n";
    expect(analysis != nullptr, "overlap fixture has an accepted analysis");
    if (analysis == nullptr) return;
    const auto model = build_genesis_executable_support_model(program, *analysis, {});
    for (const ExecutableIdentity id : std::vector<ExecutableIdentity>{0x200, 0x204, 0x206, 0x208, 0x20A, 0x20C, 0x20E, 0x210, 0x212})
      expect(in_universe(model, id), "overlap fixture: candidate in U: " + std::to_string(id));
    expect(model.roots.vector_table_present && model.roots.reset == 1U, "reset root from the vector table");
    expect(model.roots.vectors_installed == (root_into_overlap ? 1U : 0U), "installed vector roots counted");
    const auto support = compute_executable_support(model.input);
    expect(support.sound, "overlap fixture: sound run");
    expect(live(support, 0x200) && live(support, 0x204) && live(support, 0x20A) && live(support, 0x20C),
           "F7: call, continuation and callee live");
    expect(!live(support, 0x20E) && !live(support, 0x210) && !live(support, 0x212),
           "F1/F6: unsupported island and disconnected cycle removable");
    expect(support.live.size() < model.input.universe.size(),
           "F7: RTS uses the continuation authority, not whole-ROM authority");
    if (!root_into_overlap) {
      expect(!live(support, 0x206) && !live(support, 0x208), "F5: unsupported overlapping starts drop");
    } else {
      expect(live(support, 0x204) && live(support, 0x206) && live(support, 0x208),
             "F5: both overlapping starts survive when both receive support");
    }
  }
}

// 0x200 MOVE.W D1,D0 ; 0x202 JMP (0,PC,D0.W) ; ILLEGAL filler to 0x12000 ; island NOP at 0x11000.
// Current facts: the unconstrained JMP forces L = U. Operand-width model: only U ∩ [0x204-32K, 0x204+32K).
void genesis_unconstrained_and_region() {
  auto image = vector_image(0x12000);
  put(image, 0x200, {0x30, 0x01, 0x4E, 0xFB, 0x00, 0x00});
  for (std::size_t at = 0x206; at < image.size(); at += 2) put(image, at, {0x4A, 0xFC});
  put(image, 0x11000, {0x4E, 0x71});
  auto program = genesis_program(image, "synthetic/SEG-024-T001/unconstrained-region");
  expect(apply_genesis_immutable_rom_aot(program), "region fixture enumerates immutable ROM");
  const auto result = analyze_m68k_frontend(program);
  const auto *analysis = accepted_analysis(result);
  expect(analysis != nullptr, "region fixture has an accepted analysis");
  if (analysis == nullptr) return;
  const auto current = build_genesis_executable_support_model(program, *analysis, {});
  const auto all = compute_executable_support(current.input);
  expect(all.live.size() == current.input.universe.size() && live(all, 0x11000), "F3: unconstrained indirect -> L = U");
  GenesisExecutableSupportConfig width{};
  width.indirects = GenesisIndirectDomainModel::architectural_operand_width;
  const auto refined_model = build_genesis_executable_support_model(program, *analysis, width);
  const auto refined = compute_executable_support(refined_model.input);
  expect(refined.sound, "F4: operand-width refinement is a sound run");
  expect(live(refined, 0x204) && live(refined, 0x8202) && !live(refined, 0x8204) && !live(refined, 0x11000),
         "F4: only U ∩ R retained by the refined site");
  // Determinism and privacy of the sanitized report.
  const auto first = format_genesis_executable_support_report(program, *analysis);
  const auto second = format_genesis_executable_support_report(program, *analysis);
  expect(first == second, "report is byte-identical across runs");
  expect(first.find("11000") == std::string::npos && first.find("0x") == std::string::npos,
         "report carries no address");
}

}  // namespace

int main() {
  fixture1_unreachable_island();
  fixture2_exact_indirect();
  fixture3_unconstrained_indirect();
  fixture4_bounded_region();
  fixture6_circular_support();
  fixture8_anti_circularity();
  generic_determinism_and_ablation_flags();
  m68k_projection();
  genesis_overlap_island_cycle_continuation();
  genesis_unconstrained_and_region();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cout << "executable_support_tests: OK\n";
  return EXIT_SUCCESS;
}
