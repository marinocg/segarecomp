// SEG-030-T010 (ADR 0079 decision 7, record T010): the Genesis Z80 work-RAM store-freedom proof and its credited use by the report
// driver. Project-authored synthetic Z80 and MC68000 fixtures only (real encodings from the public Zilog UM0080 / M68000PRM tables);
// no commercial input.
//
// Proof fixtures (prove_genesis_z80_ram_writes):
//   1. constant stores below $8000 only: `none`; the same fixture with its store target flipped into the bank window is not `none`;
//   2. a nine-write bank select of a work-RAM bank, then a window store: `ranges` (exactly the stored byte); a ROM bank: `none`;
//      a RAM bank changed to a ROM bank before the store: `none`;
//   3. an unknown bank value (loaded from RAM): the window store may reach the Z80 area: `all`;
//   4. a (HL) store with an Unknown HL: `all` (store_target_unknown);
//   5. a PUSH with an Unknown SP: `all`; a PUSH at the reset SP ($FFFF, inside the window) under a RAM bank: `ranges`;
//   6. EI + IM 1 with an interrupt handler at $0038 storing through the window: `all` under an unknown bank, `none` under a ROM bank
//      (the RETI returns to the interrupted points); IM 2: `all` (interrupt_mode_unbounded);
//   7. JP (HL) with an Unknown HL: `all`; with a constant HL: bounded (`none`);
//   8. a 68K store into an operand byte of a reachable instruction while the Z80 may run: `all`; the same store under /RESET (part
//      of the image) or into a data byte: `none`;
//   9. a 68K Unknown-target store while the Z80 may run: `all`; under /RESET: `none`; a 68K bank-register store makes the latch
//      Unknown (the ROM-bank window store becomes `all`);
//  10. CALL/RET balanced: `none`; RET without a pushed slot: `all` (return_unbounded); a Z80 store into its own code: `all`;
//  11. image set unknown: `all`; deterministic outputs.
// Driver fixtures (run_genesis_analysis_report, frames domain): a 68K program that stores a work-RAM byte, releases the Z80 and reads
// the byte back. Unknown image set: blanket external writer (read Unknown external_writer). Proven-free image: credited bound, no
// external writer, the read is precise. A Z80 image writing exactly that byte: the read is Unknown(async_writer); writing another
// byte: precise. A 68K store into the image's code while the Z80 may run: blanket again.

#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "segarecomp/genesis_analysis_report/report.hpp"
#include "segarecomp/genesis_analysis_report/z80_ram_write_proof.hpp"

namespace {

using namespace segarecomp;
using Reason = GenesisZ80ProofReason;
using Writes = GenesisZ80RamWrites;

int failures = 0;
void expect(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

// A tiny Z80 assembler (Zilog UM0080 encodings).
struct Z80 {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x200U, 0x00U);
  std::uint16_t pc{};
  Z80 &at(std::uint16_t address) {
    pc = address;
    return *this;
  }
  Z80 &b(std::initializer_list<std::uint8_t> values) {
    for (const auto value : values) bytes.at(pc++) = value;
    return *this;
  }
  static std::uint8_t lo(std::uint16_t v) { return static_cast<std::uint8_t>(v); }
  static std::uint8_t hi(std::uint16_t v) { return static_cast<std::uint8_t>(v >> 8U); }
  Z80 &ld_sp(std::uint16_t v) { return b({0x31, lo(v), hi(v)}); }
  Z80 &ld_hl(std::uint16_t v) { return b({0x21, lo(v), hi(v)}); }
  Z80 &ld_a(std::uint8_t v) { return b({0x3E, v}); }
  Z80 &ld_mem_a(std::uint16_t address) { return b({0x32, lo(address), hi(address)}); }
  Z80 &ld_a_mem(std::uint16_t address) { return b({0x3A, lo(address), hi(address)}); }
  Z80 &ld_hl_ind_a() { return b({0x77}); }
  Z80 &ld_h_a() { return b({0x67}); }
  Z80 &ld_hl_mem(std::uint16_t address) { return b({0x2A, lo(address), hi(address)}); }
  Z80 &ld_sp_hl() { return b({0xF9}); }
  Z80 &push_bc() { return b({0xC5}); }
  Z80 &jr_self() { return b({0x18, 0xFE}); }
  Z80 &jp(std::uint16_t to) { return b({0xC3, lo(to), hi(to)}); }
  Z80 &jp_hl() { return b({0xE9}); }
  Z80 &call(std::uint16_t to) { return b({0xCD, lo(to), hi(to)}); }
  Z80 &ret() { return b({0xC9}); }
  Z80 &ei() { return b({0xFB}); }
  Z80 &im(unsigned mode) { return b({0xED, static_cast<std::uint8_t>(mode == 0U ? 0x46 : mode == 1U ? 0x56 : 0x5E)}); }
  Z80 &reti() { return b({0xED, 0x4D}); }
  // Nine writes of bit 0 of `bank`'s bits (LSB first) to the bank register: the latch then holds `bank`.
  Z80 &select_bank(std::uint16_t bank) {
    for (unsigned i = 0; i < 9U; ++i) ld_a(static_cast<std::uint8_t>((bank >> i) & 1U)).ld_mem_a(0x6000U);
    return *this;
  }
  [[nodiscard]] std::vector<GenesisZ80Image> image() const { return {GenesisZ80Image{bytes, "synthetic"}}; }
};

GenesisZ80RamWriteProof prove(const Z80 &z, std::vector<GenesisZ80AreaStores> stores = {}) {
  return prove_genesis_z80_ram_writes(z.image(), stores);
}

bool has(const GenesisZ80RamWriteProof &proof, Reason reason) { return proof.reasons.contains(reason); }

std::string describe(const GenesisZ80RamWriteProof &proof) {
  return format_genesis_z80_ram_write_proof(proof, "test");
}

bool exactly(const GenesisZ80RamWriteProof &proof, std::uint32_t lo, std::uint32_t hi) {
  return proof.outcome == Writes::ranges && proof.reasons.empty() && proof.work_ram.size() == 1U &&
         proof.work_ram[0].kind == M68kRegionKind::mutable_ram && proof.work_ram[0].lo == lo && proof.work_ram[0].hi == hi;
}

// 1. Constant stores below $8000.
Z80 proven_free(std::uint16_t target = 0x1000U) {
  Z80 z;
  z.ld_sp(0x1FF0U).ld_a(0x2A).ld_mem_a(target).ld_hl(0x1100U).ld_hl_ind_a().jr_self();
  return z;
}

void constant_stores() {
  const auto free = prove(proven_free());
  expect(free.outcome == Writes::none && free.reasons.empty() && free.work_ram.empty() && free.reachable_instructions == 6U &&
             free.store_sites == 2U && free.window_store_sites == 0U && free.bound() && free.bound()->empty(),
         "proven-free: constant stores below $8000 are `none`: " + describe(free));
  const auto flipped = prove(proven_free(0x9000U));
  expect(flipped.outcome == Writes::all && has(flipped, Reason::window_store_into_z80_area) && flipped.window_store_sites == 1U,
         "proven-free mutated: the store target flipped into the window (unknown bank) is not `none`: " + describe(flipped));
  expect(describe(free) == describe(prove(proven_free())) && describe(flipped) == describe(prove(proven_free(0x9000U))),
         "deterministic proof outputs");
  expect(free.image_hashes.size() == 1U && free.image_hashes[0].size() == 64U, "the image content hash is recorded");
}

// 2. Bank selection.
void bank_selection() {
  Z80 ram;
  ram.ld_sp(0x1FF0U).select_bank(0x1FFU).ld_a(1).ld_mem_a(0x8123U).jr_self();
  const auto to_ram = prove(ram);
  expect(exactly(to_ram, 0x8123U, 0x8124U) && to_ram.bank_register_store_sites == 9U && to_ram.window_store_sites == 1U,
         "bank $1FF then a window store: exactly the stored work-RAM byte: " + describe(to_ram));
  expect(to_ram.bound() && to_ram.bound()->size() == 1U, "the `ranges` bound is the M68K input");

  Z80 low;
  low.ld_sp(0x1FF0U).select_bank(0x1C0U).ld_a(1).ld_mem_a(0x8123U).jr_self();
  expect(exactly(prove(low), 0x0123U, 0x0124U), "bank $1C0 (the lowest work-RAM mirror) selects work RAM: " + describe(prove(low)));

  Z80 rom;
  rom.ld_sp(0x1FF0U).select_bank(0x001U).ld_a(1).ld_mem_a(0x8123U).jr_self();
  const auto to_rom = prove(rom);
  expect(to_rom.outcome == Writes::none && to_rom.reasons.empty(), "a ROM bank then a window store: `none`: " + describe(to_rom));

  Z80 changed;
  changed.ld_sp(0x1FF0U).select_bank(0x1FFU).select_bank(0x002U).ld_a(1).ld_mem_a(0x8123U).jr_self();
  const auto after_change = prove(changed);
  expect(after_change.outcome == Writes::none, "a RAM bank changed to a ROM bank before the store: `none`: " + describe(after_change));

  Z80 partial;  // eight writes only: bit 0 of the latch is still the unknown entry bit 8 -> bank $1FE or $1FF
  partial.ld_sp(0x1FF0U);
  for (unsigned i = 0; i < 8U; ++i) partial.ld_a(1).ld_mem_a(0x6000U);
  partial.ld_mem_a(0x8123U).jr_self();
  const auto both = prove(partial);
  expect(both.outcome == Writes::ranges && both.work_ram.size() == 2U && both.work_ram[0].lo == 0x0123U &&
             both.work_ram[0].hi == 0x0124U && both.work_ram[1].lo == 0x8123U && both.work_ram[1].hi == 0x8124U,
         "a partially known latch: every RAM completion: " + describe(both));
}

// 3. Unknown bank value.
void unknown_bank() {
  Z80 z;
  z.ld_sp(0x1FF0U);
  for (unsigned i = 0; i < 9U; ++i) z.ld_a_mem(0x1000U).ld_mem_a(0x6000U);
  z.ld_a(1).ld_mem_a(0x8123U).jr_self();
  const auto proof = prove(z);
  expect(proof.outcome == Writes::all && has(proof, Reason::window_store_into_z80_area) && proof.work_ram.empty() && !proof.bound(),
         "an unknown bank value: `all`: " + describe(proof));
}

// 4. (HL) store with an Unknown HL.
void unknown_hl() {
  Z80 z;
  z.ld_sp(0x1FF0U).ld_a_mem(0x1000U).ld_h_a().ld_hl_ind_a().jr_self();
  const auto proof = prove(z);
  expect(proof.outcome == Writes::all && has(proof, Reason::store_target_unknown), "(HL) with an Unknown HL: `all`: " + describe(proof));
}

// 5. Stack pushes.
void stack_pushes() {
  Z80 unknown;
  unknown.ld_hl_mem(0x1000U).ld_sp_hl().push_bc().jr_self();
  const auto proof = prove(unknown);
  expect(proof.outcome == Writes::all && has(proof, Reason::stack_pointer_unknown), "PUSH with an Unknown SP: `all`: " + describe(proof));

  Z80 window;  // the reset SP is $FFFF: the push writes $FFFD-$FFFE through the window
  window.select_bank(0x1FFU).push_bc().jr_self();
  expect(exactly(prove(window), 0xFFFDU, 0xFFFFU), "PUSH at the reset SP under a RAM bank: the two stacked bytes: " +
                                                       describe(prove(window)));
}

// 6. Interrupts.
Z80 interrupt_fixture(unsigned mode, std::optional<std::uint16_t> bank) {
  Z80 z;
  z.jp(0x0100U);
  z.at(0x0038U).ld_a(1).ld_mem_a(0x8040U).ei().reti();
  z.at(0x0100U).ld_sp(0x1F00U);
  if (bank) z.select_bank(*bank);
  z.im(mode).ei().jr_self();
  return z;
}

void interrupts() {
  const auto unknown = prove(interrupt_fixture(1U, std::nullopt));
  expect(unknown.outcome == Writes::all && has(unknown, Reason::window_store_into_z80_area) && unknown.interrupt_entries > 0U,
         "EI + IM 1, handler window store under an unknown bank: `all`: " + describe(unknown));
  const auto rom = prove(interrupt_fixture(1U, 0x003U));
  expect(rom.outcome == Writes::none && rom.reasons.empty() && rom.interrupt_entries > 0U,
         "EI + IM 1 under a ROM bank: the handler and its RETI are bounded: " + describe(rom));
  const auto ram = prove(interrupt_fixture(1U, 0x1FEU));
  expect(exactly(ram, 0x0040U, 0x0041U), "EI + IM 1 under a RAM bank: the handler's byte: " + describe(ram));
  const auto im0 = prove(interrupt_fixture(0U, 0x1FEU));
  expect(exactly(im0, 0x0040U, 0x0041U), "IM 0 (acknowledge byte $FF = RST 38h) enters the same handler: " + describe(im0));
  const auto im2 = prove(interrupt_fixture(2U, 0x003U));
  expect(im2.outcome == Writes::all && has(im2, Reason::interrupt_mode_unbounded), "IM 2: `all`: " + describe(im2));
}

// 7. Computed jumps.
void computed_jumps() {
  Z80 unknown;
  unknown.ld_sp(0x1FF0U).ld_hl_mem(0x1000U).jp_hl();
  const auto proof = prove(unknown);
  expect(proof.outcome == Writes::all && has(proof, Reason::indirect_control), "JP (HL) with an Unknown HL: `all`: " + describe(proof));
  Z80 known;
  known.ld_sp(0x1FF0U).ld_hl(0x0040U).jp_hl();
  known.at(0x0040U).ld_a(1).ld_mem_a(0x1000U).jr_self();
  const auto bounded = prove(known);
  expect(bounded.outcome == Writes::none && bounded.reachable_instructions == 6U, "JP (HL) with a constant HL: bounded: " +
                                                                                      describe(bounded));
}

// 8. and 9. 68K stores into the Z80 area.
void m68k_stores() {
  const auto z = proven_free();  // LD SP,nn at $0000: operand bytes $0001-$0002
  const auto patched = prove(z, {GenesisZ80AreaStores{{{0xA00001U, 0xA00002U}}, 0U, false}});
  expect(patched.outcome == Writes::all && has(patched, Reason::m68k_store_into_z80_code),
         "a 68K store into an operand byte while the Z80 may run: `all`: " + describe(patched));
  const auto mirror = prove(z, {GenesisZ80AreaStores{{{0xA02001U, 0xA02002U}}, 0U, false}});
  expect(mirror.outcome == Writes::all && has(mirror, Reason::m68k_store_into_z80_code), "the RAM mirror is the same byte");
  const auto in_reset = prove(z, {GenesisZ80AreaStores{{{0xA00000U, 0xA02000U}}, 0U, true}});
  expect(in_reset.outcome == Writes::none && in_reset.m68k_known_ranges == 1U,
         "68K stores under /RESET are the image itself: `none`: " + describe(in_reset));
  const auto data = prove(z, {GenesisZ80AreaStores{{{0xA01800U, 0xA01801U}}, 0U, false}});
  expect(data.outcome == Writes::none, "a 68K store into a data byte while the Z80 may run: `none`: " + describe(data));

  const auto unknown = prove(z, {GenesisZ80AreaStores{{}, 3U, false}});
  expect(unknown.outcome == Writes::all && has(unknown, Reason::m68k_store_into_z80_ram_unbounded) && unknown.bank_volatile &&
             unknown.m68k_unknown_target_stores == 3U,
         "a 68K Unknown-target store while the Z80 may run: `all`: " + describe(unknown));
  const auto unknown_reset = prove(z, {GenesisZ80AreaStores{{}, 3U, true}});
  expect(unknown_reset.outcome == Writes::none && unknown_reset.bank_volatile, "the same store under /RESET: `none`");

  Z80 rom;
  rom.ld_sp(0x1FF0U).select_bank(0x001U).ld_a(1).ld_mem_a(0x8123U).jr_self();
  const auto volatile_bank = prove(rom, {GenesisZ80AreaStores{{{0xA06000U, 0xA06001U}}, 0U, true}});
  expect(volatile_bank.outcome == Writes::all && volatile_bank.bank_volatile,
         "a 68K bank-register store makes the latch Unknown at every Z80 point: " + describe(volatile_bank));
}

// 10. Calls, returns and self-modification.
void calls_and_self_modification() {
  Z80 balanced;
  balanced.ld_sp(0x1F00U).call(0x0040U).call(0x0040U).jr_self();
  balanced.at(0x0040U).ld_a(1).ld_mem_a(0x1000U).ret();
  const auto proof = prove(balanced);
  expect(proof.outcome == Writes::none && proof.reasons.empty() && proof.reachable_instructions == 7U,
         "CALL/RET from two sites: bounded: " + describe(proof));
  const auto slot = prove(balanced, {GenesisZ80AreaStores{{{0xA01EFEU, 0xA01EFFU}}, 0U, false}});
  expect(slot.outcome == Writes::all && has(slot, Reason::m68k_store_into_z80_code),
         "a 68K store into a read return slot while the Z80 may run: `all`: " + describe(slot));

  Z80 bare;
  bare.ld_sp(0x1F00U).ret();
  const auto unbounded = prove(bare);
  expect(unbounded.outcome == Writes::all && has(unbounded, Reason::return_unbounded), "RET without a pushed slot: `all`");

  Z80 self;
  self.ld_sp(0x1F00U).ld_a(0).ld_mem_a(0x0001U).jr_self();
  const auto modified = prove(self);
  expect(modified.outcome == Writes::all && has(modified, Reason::self_modifying_store),
         "a Z80 store into its own operand byte: `all`: " + describe(modified));

  Z80 runaway;  // falls off the end of the image: no bytes are invented
  runaway.bytes.resize(4U);
  runaway.ld_sp(0x1F00U);
  expect(prove(runaway).outcome == Writes::all && has(prove(runaway), Reason::undecodable_code), "code beyond the image: `all`");
}

// 11. The image set.
void image_set() {
  const auto unknown = prove_genesis_z80_ram_writes(std::nullopt, {});
  expect(unknown.outcome == Writes::all && has(unknown, Reason::image_set_unknown) && !unknown.bound(),
         "image set unknown: `all`: " + describe(unknown));
  const auto empty = prove_genesis_z80_ram_writes(std::vector<GenesisZ80Image>{}, {});
  expect(empty.outcome == Writes::all && has(empty, Reason::image_set_unknown), "an empty image set is not a known set");
  const auto oversized = prove_genesis_z80_ram_writes(std::vector<GenesisZ80Image>{{std::vector<std::uint8_t>(0x2001U, 0U), "x"}}, {});
  expect(oversized.outcome == Writes::all && has(oversized, Reason::image_invalid), "an image larger than Z80 RAM is invalid");
  Z80 ram;
  ram.ld_sp(0x1FF0U).select_bank(0x1FFU).ld_a(1).ld_mem_a(0x8123U).jr_self();
  auto both = proven_free().image();
  both.push_back(ram.image().front());
  expect(exactly(prove_genesis_z80_ram_writes(both, {}), 0x8123U, 0x8124U), "two images: the union of their writes");
}

// ---------------------------------------------------------------------------------------------------------------
// Driver: the credited bound replaces the blanket external-writer rule only when the proof holds over the run's own stores.

struct M68k {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x400U, 0U);
  std::uint32_t pc = 0x200U;
  M68k() {
    put32(0U, 0x00FFFE00U);
    put32(4U, 0x200U);
  }
  void put32(std::uint32_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4U; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (24U - 8U * i));
  }
  M68k &w(std::uint32_t value) {
    bytes[pc] = static_cast<std::uint8_t>(value >> 8U);
    bytes[pc + 1U] = static_cast<std::uint8_t>(value);
    pc += 2U;
    return *this;
  }
  M68k &l(std::uint32_t value) { return w(value >> 16U).w(value & 0xFFFFU); }
};

// move.b #$2A,$FF0010 ; [move.b #0,<z80 store>] ; move.w #$100,$A11200 (release) ; move.b $FF0010,d0 ; bra.s *
std::optional<FrontendProgram> driver_program(std::optional<std::uint32_t> z80_store) {
  M68k m;
  m.w(0x13FCU).w(0x002AU).l(0xFF0010U);
  if (z80_store) m.w(0x13FCU).w(0x0000U).l(*z80_store);
  m.w(0x33FCU).w(0x0100U).l(0xA11200U);
  m.w(0x1039U).l(0xFF0010U);
  m.w(0x60FEU);
  auto program = make_genesis_bridge_startup_program(m.bytes, 0U, 0x200U, std::nullopt);
  if (!program || !apply_genesis_immutable_rom_aot(*program)) return std::nullopt;
  return program;
}

std::size_t reads_with(const GenesisAnalysisReport &report, M68kAnalysisSubReason sub) {
  std::size_t n = 0U;
  for (const auto &[key, count] : report.analysis.memory.unknown_reads)
    if (key.second == sub) n += count;
  return n;
}

void driver() {
  const auto program = driver_program(std::nullopt);
  expect(program.has_value(), "driver fixture program");
  if (!program) return;
  GenesisAnalysisReportConfig config{};
  config.domains.frames = true;
  config.reset_entry = true;

  const auto blanket = run_genesis_analysis_report(*program, config);
  expect(blanket.analysis.complete && blanket.analysis.memory.release_store && blanket.analysis.memory.policy.external_writer &&
             blanket.z80_proof && blanket.z80_proof->outcome == Writes::all && has(*blanket.z80_proof, Reason::image_set_unknown) &&
             !blanket.z80_bound_credited && blanket.z80_proof_runs == 1U &&
             reads_with(blanket, M68kAnalysisSubReason::external_writer) == 1U && blanket.analysis.memory.precise_reads == 0U,
         "unknown image set: the blanket external writer; the work-RAM read is Unknown(external_writer)");
  const auto aggregate = format_genesis_analysis_report_aggregate(blanket, config);
  expect(aggregate.find("\"z80_ram_write_proof\":{\"outcome\":\"all\",\"reasons\":[\"image_set_unknown\"],\"credited_bound\":"
                        "\"blanket\"") != std::string::npos,
         "the aggregate reports the proof outcome, reasons and the blanket rule: " + aggregate);

  auto free_config = config;
  free_config.z80_images = proven_free().image();
  const auto free = run_genesis_analysis_report(*program, free_config);
  expect(free.analysis.complete && free.analysis.memory.release_store && !free.analysis.memory.policy.external_writer &&
             free.z80_proof && free.z80_proof->outcome == Writes::none && free.z80_bound_credited &&
             free.analysis.memory.precise_reads == 1U && reads_with(free, M68kAnalysisSubReason::external_writer) == 0U,
         "a proven-free image: the credited bound removes the external writer; the read is precise");
  expect(format_genesis_analysis_report_aggregate(free, free_config).find("\"outcome\":\"none\",\"reasons\":[],\"credited_bound\":"
                                                                         "\"proof\"") != std::string::npos,
         "the aggregate marks the credited proof bound");

  Z80 hit;
  hit.ld_sp(0x1FF0U).select_bank(0x1FEU).ld_a(1).ld_mem_a(0x8010U).jr_self();  // 68K $FF0010
  auto hit_config = config;
  hit_config.z80_images = hit.image();
  const auto hit_report = run_genesis_analysis_report(*program, hit_config);
  expect(hit_report.z80_proof && hit_report.z80_proof->outcome == Writes::ranges && hit_report.z80_bound_credited &&
             !hit_report.analysis.memory.policy.external_writer && hit_report.analysis.memory.precise_reads == 0U &&
             reads_with(hit_report, M68kAnalysisSubReason::async_writer) == 1U,
         "a Z80 image writing the read byte: the read is Unknown(async_writer), never precise");

  Z80 miss;
  miss.ld_sp(0x1FF0U).select_bank(0x1FEU).ld_a(1).ld_mem_a(0x8011U).jr_self();
  auto miss_config = config;
  miss_config.z80_images = miss.image();
  const auto miss_report = run_genesis_analysis_report(*program, miss_config);
  expect(miss_report.z80_proof && miss_report.z80_proof->outcome == Writes::ranges && miss_report.z80_bound_credited &&
             miss_report.analysis.memory.precise_reads == 1U,
         "a Z80 image writing another byte: the read stays precise");

  const auto patching = driver_program(0xA00001U);  // a 68K store into the image's LD SP operand while the Z80 may run
  expect(patching.has_value(), "patching fixture program");
  if (!patching) return;
  const auto patched = run_genesis_analysis_report(*patching, free_config);
  expect(patched.z80_proof && patched.z80_proof->outcome == Writes::all &&
             has(*patched.z80_proof, Reason::m68k_store_into_z80_code) && !patched.z80_bound_credited &&
             patched.analysis.memory.policy.external_writer && patched.z80_proof_runs == 2U &&
             reads_with(patched, M68kAnalysisSubReason::external_writer) == 1U,
         "a 68K store into the image's code: the optimistic bound is not validated; the blanket rule is used");

  auto ablation = config;
  ablation.assume_no_z80_ram_writes = true;
  const auto ablated = run_genesis_analysis_report(*program, ablation);
  expect(ablated.z80_proof && ablated.z80_proof->outcome == Writes::all && !ablated.z80_bound_credited &&
             format_genesis_analysis_report_aggregate(ablated, ablation).find("\"credited_bound\":\"ablation\"") != std::string::npos,
         "the diagnostic ablation never credits the proof");
}

}  // namespace

int main() {
  constant_stores();
  bank_selection();
  unknown_bank();
  unknown_hl();
  stack_pushes();
  interrupts();
  computed_jumps();
  m68k_stores();
  calls_and_self_modification();
  image_set();
  driver();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cout << "analysis_genesis_z80_proof_test: all checks passed\n";
  return EXIT_SUCCESS;
}
