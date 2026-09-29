// SEG-026-T001: reachability-first discovery challenger fixtures (project-authored, ROM-independent).
//
// Every fixture is a small synthetic Genesis image: a vector table (reset PC = 0x200) followed by real MC68000
// encodings decoded by the unchanged decoder/lifter, so exact synthetic addresses may be asserted.

#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

#include "segarecomp/machine/genesis/reachability_challenger.hpp"

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

struct Image {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x800U, 0U);
  Image() {
    put32(0x0U, 0x00FFFE00U);  // reset SSP
    put32(0x4U, entry);        // reset PC
  }
  void put32(std::uint32_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4U; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (24U - 8U * i));
  }
  // Writes big-endian 16-bit words starting at `at`.
  void words(std::uint32_t at, std::initializer_list<std::uint16_t> values) {
    for (const auto value : values) {
      bytes[at] = static_cast<std::uint8_t>(value >> 8U);
      bytes[at + 1U] = static_cast<std::uint8_t>(value);
      at += 2U;
    }
  }
  void vector(unsigned number, std::uint32_t handler) { put32(number * 4U, handler); }
};

GenesisReachabilityChallengerResult run(const Image &image,
                                        GenesisReachabilityExceptionModel model = GenesisReachabilityExceptionModel::strict,
                                        bool pea = false) {
  const auto program = make_genesis_bridge_startup_program(image.bytes, 0U, entry, std::nullopt);
  if (!program) {
    ++failures;
    std::cerr << "FAIL: fixture program\n";
    return {};
  }
  GenesisReachabilityChallengerConfig config{};
  config.exception_model = model;
  config.pea_continuations = pea;
  return run_genesis_reachability_challenger(*program, config);
}

bool in_d(const GenesisReachabilityChallengerResult &r, std::uint32_t pc) { return r.discovered.contains(pc); }
bool site(const GenesisReachabilityChallengerResult &r, M68kDynamicControlFamily family, std::uint32_t pc) {
  return r.sites[static_cast<std::size_t>(family)].contains(pc);
}

constexpr std::uint16_t nop = 0x4E71U, rts = 0x4E75U, rte = 0x4E73U;
std::uint16_t bra_s(std::uint32_t from, std::uint32_t to) { return static_cast<std::uint16_t>(0x6000U | ((to - from - 2U) & 0xFFU)); }
std::uint16_t beq_s(std::uint32_t from, std::uint32_t to) { return static_cast<std::uint16_t>(0x6700U | ((to - from - 2U) & 0xFFU)); }
std::uint16_t bsr_s(std::uint32_t from, std::uint32_t to) { return static_cast<std::uint16_t>(0x6100U | ((to - from - 2U) & 0xFFU)); }

// Common data island at 0x600: legal decodes (NOP; NOP; RTS; BSR.S) that nothing reaches.
void add_island(Image &image) {
  image.words(0x600U, {nop, nop, rts, bsr_s(0x606U, 0x600U), nop});
}

// Fixture 1: root + direct CFG (fallthrough, BRA, JMP abs.l, a self loop).
void fixture1_direct_cfg() {
  Image image;
  image.words(0x200U, {nop, bra_s(0x202U, 0x210U)});
  image.words(0x210U, {0x4EF9U, 0x0000U, 0x0220U});  // JMP $00000220
  image.words(0x220U, {nop, bra_s(0x222U, 0x220U)});
  add_island(image);
  const auto r = run(image);
  expect(r.discovered.size() == 5U && in_d(r, 0x200U) && in_d(r, 0x202U) && in_d(r, 0x210U) && in_d(r, 0x220U) &&
             in_d(r, 0x222U),
         "F1: exactly the root's direct CFG is discovered");
  expect(!in_d(r, 0x216U), "F1: bytes after an unconditional JMP are not swept");
  expect(r.roots.size() == 1U && r.roots.front() == entry, "F1: reset entry is the only root without vectors");
}

// Fixture 2: an unreachable island of legal decodes never enters D (the crucial regression).
void fixture2_unreachable_island() {
  Image image;
  image.words(0x200U, {bra_s(0x200U, 0x200U)});
  add_island(image);
  const auto r = run(image);
  for (std::uint32_t pc = 0x600U; pc < 0x60AU; pc += 2U) expect(!in_d(r, pc), "F2: legal island decode must not enter D");
  expect(r.discovered.size() == 1U, "F2: only the root loop");
}

// Fixture 3: both conditional outcomes.
void fixture3_conditional() {
  Image image;
  image.words(0x200U, {beq_s(0x200U, 0x210U), bra_s(0x202U, 0x202U)});
  image.words(0x210U, {bra_s(0x210U, 0x210U)});
  const auto r = run(image);
  expect(in_d(r, 0x202U) && in_d(r, 0x210U) && r.discovered.size() == 3U, "F3: taken and not-taken outcomes");
  // DBF D0: both outcomes as well.
  Image dbcc;
  dbcc.words(0x200U, {0x51C8U, 0x000EU, bra_s(0x204U, 0x204U)});  // DBF D0,$210
  dbcc.words(0x210U, {bra_s(0x210U, 0x210U)});
  const auto d = run(dbcc);
  expect(in_d(d, 0x204U) && in_d(d, 0x210U), "F3: DBcc both outcomes");
}

// Fixture 4: direct call + RTS resumes the discovered call's continuation.
void fixture4_call_rts() {
  Image image;
  image.words(0x200U, {bsr_s(0x200U, 0x210U), nop, bra_s(0x204U, 0x204U)});
  image.words(0x210U, {nop, rts});
  const auto r = run(image);
  expect(in_d(r, 0x210U) && in_d(r, 0x212U), "F4: callee discovered");
  expect(r.call_continuations.contains(0x202U) && in_d(r, 0x202U) && in_d(r, 0x204U), "F4: continuation resumed");
  expect(r.continuations_enabled && site(r, M68kDynamicControlFamily::return_from_subroutine, 0x212U), "F4: RTS site");
  // A direct JSR abs.l call behaves identically.
  Image jsr;
  jsr.words(0x200U, {0x4EB9U, 0x0000U, 0x0210U, bra_s(0x206U, 0x206U)});
  jsr.words(0x210U, {rts});
  const auto j = run(jsr);
  expect(in_d(j, 0x206U) && j.call_continuations.contains(0x206U), "F4: JSR abs.l continuation");
  // Without any RTS, a continuation is never resumed.
  Image noreturn;
  noreturn.words(0x200U, {bsr_s(0x200U, 0x210U), nop});
  noreturn.words(0x210U, {bra_s(0x210U, 0x210U)});
  const auto n = run(noreturn);
  expect(!n.continuations_enabled && !in_d(n, 0x202U), "F4: no reachable RTS, no continuation");
}

// Fixture 5: multiple discovered calls sharing one RTS both resume.
void fixture5_shared_rts() {
  Image image;
  image.words(0x200U, {bsr_s(0x200U, 0x220U), bsr_s(0x202U, 0x220U), bra_s(0x204U, 0x204U)});
  image.words(0x220U, {rts});
  const auto r = run(image);
  expect(in_d(r, 0x202U) && in_d(r, 0x204U) && r.call_continuations.size() == 2U, "F5: both continuations");
}

// Fixture 6: an unreachable fake call must not contribute an RTS continuation.
void fixture6_fake_call() {
  Image image;
  image.words(0x200U, {bsr_s(0x200U, 0x220U), bra_s(0x202U, 0x202U)});
  image.words(0x220U, {rts});
  add_island(image);  // contains BSR.S at 0x606 whose continuation is 0x608
  const auto r = run(image);
  expect(!r.call_continuations.contains(0x608U) && !in_d(r, 0x608U) && !in_d(r, 0x606U),
         "F6: an unreachable call contributes nothing");
  expect(r.call_continuations.size() == 1U, "F6: only the discovered call's continuation");
}

// Fixture 7: a disconnected cycle is not self-justifying.
void fixture7_disconnected_cycle() {
  Image image;
  image.words(0x200U, {bra_s(0x200U, 0x200U)});
  image.words(0x500U, {bra_s(0x500U, 0x502U), bra_s(0x502U, 0x500U)});
  const auto r = run(image);
  expect(!in_d(r, 0x500U) && !in_d(r, 0x502U), "F7: disconnected cycle absent");
}

// Fixture 8: JMP (A0) and JSR (A0) are recorded and discovery stops through them.
void fixture8_address_indirect() {
  Image image;
  image.words(0x200U, {beq_s(0x200U, 0x204U), 0x4ED0U, 0x4E90U, nop});  // BEQ.S; JMP (A0); JSR (A0); NOP
  image.words(0x300U, {rts});
  const auto r = run(image);
  expect(site(r, M68kDynamicControlFamily::jump_address_indirect, 0x202U), "F8: JMP (An) site");
  expect(site(r, M68kDynamicControlFamily::call_address_indirect, 0x204U), "F8: JSR (An) site");
  expect(r.call_continuations.contains(0x206U), "F8: indirect call still stacks its continuation");
  expect(!in_d(r, 0x300U) && r.discovered.size() == 3U, "F8: no target is guessed; no sweep past JMP");
  expect(!r.continuations_enabled && !in_d(r, 0x206U), "F8: continuation needs a reachable RTS");
}

// Fixture 9: JMP (d8,PC,D0.W) is an unresolved PC-indexed site; its table is never decoded.
void fixture9_pc_indexed() {
  Image image;
  image.words(0x200U, {0x4EFBU, 0x0002U, bra_s(0x204U, 0x210U), bra_s(0x206U, 0x220U)});  // JMP (2,PC,D0.W)
  image.words(0x210U, {bra_s(0x210U, 0x210U)});
  image.words(0x220U, {bra_s(0x220U, 0x220U)});
  const auto r = run(image);
  expect(site(r, M68kDynamicControlFamily::jump_pc_index, 0x200U), "F9: PC-indexed site recorded");
  expect(r.discovered.size() == 1U && !in_d(r, 0x204U) && !in_d(r, 0x210U), "F9: no table recovery, no sweep");
}

// Fixtures 10/11: RTE under the strict model and the normal-resumption hypothesis. The IRQ6 handler and the
// TRAP #0 handler are vector roots; TRAP's stacked continuation is only reached by an exception return.
Image exception_image() {
  Image image;
  image.vector(30U, 0x300U);  // IRQ6
  image.vector(32U, 0x320U);  // TRAP #0
  image.words(0x200U, {0x4E40U, nop, bra_s(0x204U, 0x204U)});  // TRAP #0; NOP; BRA self
  image.words(0x300U, {nop, rte});
  image.words(0x320U, {rte});
  image.words(0x340U, {nop, rts});  // never referenced
  return image;
}

void fixture10_rte_strict() {
  const auto r = run(exception_image());
  expect(r.roots.size() == 3U && r.vector_roots == 2U, "F10: reset + delivered vectors are roots");
  expect(in_d(r, 0x300U) && in_d(r, 0x302U) && in_d(r, 0x320U), "F10: handlers discovered from vectors");
  expect(site(r, M68kDynamicControlFamily::return_from_exception, 0x302U), "F10: RTE site");
  expect(r.exception_continuations.contains(0x202U) && !in_d(r, 0x202U), "F10: strict RTE discovers nothing");
}

void fixture11_rte_normal_resumption() {
  const auto strict = run(exception_image());
  const auto normal = run(exception_image(), GenesisReachabilityExceptionModel::normal_resumption);
  expect(in_d(normal, 0x202U) && in_d(normal, 0x204U), "F11: TRAP continuation resumed under the hypothesis");
  std::vector<std::uint32_t> added;
  for (const auto &[pc, length] : normal.discovered)
    if (!strict.discovered.contains(pc)) added.push_back(pc);
  expect(added == std::vector<std::uint32_t>({0x202U, 0x204U}),
         "F11: the hypothesis adds only already-stacked resumption boundaries and their fixed flow");
  expect(!in_d(normal, 0x340U), "F11: RTE never broadens to unreferenced code");
}

// Fixture 12: push-then-RTS is a computed jump (ADR 0048), not an ordinary call return.
void fixture12_push_then_rts() {
  Image image;
  image.words(0x200U, {bsr_s(0x200U, 0x210U), bra_s(0x202U, 0x202U)});
  image.words(0x210U, {0x2F3CU, 0x0000U, 0x0300U, rts});  // MOVE.L #$300,-(A7); RTS
  image.words(0x300U, {bra_s(0x300U, 0x300U)});
  const auto r = run(image);
  expect(r.sites[genesis_challenger_family_push_window_rts].contains(0x216U),
         "F12: RTS classified as push-then-RTS");
  expect(!site(r, M68kDynamicControlFamily::return_from_subroutine, 0x216U), "F12: not an ordinary RTS");
  expect(!in_d(r, 0x300U), "F12: the pushed target is not followed");
  expect(!r.continuations_enabled && !in_d(r, 0x202U), "F12: a computed-jump RTS does not resume call continuations");
  // PEA of a code address is only a continuation in the explicit variant.
  Image pea;
  pea.words(0x200U, {0x4879U, 0x0000U, 0x0220U, 0x4EF9U, 0x0000U, 0x0230U});  // PEA $220; JMP $230
  pea.words(0x220U, {bra_s(0x220U, 0x220U)});
  pea.words(0x230U, {rts});
  expect(!in_d(run(pea), 0x220U), "F12: PEA target is not a continuation by default");
  expect(in_d(run(pea, GenesisReachabilityExceptionModel::strict, true), 0x220U), "F12: PEA continuation variant");
}

void report_is_aggregate_only() {
  const auto r = run(exception_image());
  GenesisReachabilityChallengerConfig config{};
  const auto aggregate = format_genesis_reachability_challenger_aggregate(r, config);
  expect(aggregate.find("\"discovered\":") != std::string::npos && aggregate.find("0x") == std::string::npos &&
             aggregate.find("000300") == std::string::npos,
         "aggregate report carries counts only");
  const auto priv = format_genesis_reachability_challenger_private(r, config);
  expect(priv.find("\"000300\"") != std::string::npos, "private report carries exact PCs");
  expect(run(exception_image()).discovered == r.discovered, "deterministic");
}

}  // namespace

int main() {
  fixture1_direct_cfg();
  fixture2_unreachable_island();
  fixture3_conditional();
  fixture4_call_rts();
  fixture5_shared_rts();
  fixture6_fake_call();
  fixture7_disconnected_cycle();
  fixture8_address_indirect();
  fixture9_pc_indexed();
  fixture10_rte_strict();
  fixture11_rte_normal_resumption();
  fixture12_push_then_rts();
  report_is_aggregate_only();
  if (failures != 0) return EXIT_FAILURE;
  std::cout << "reachability_challenger_tests: OK\n";
  return EXIT_SUCCESS;
}
