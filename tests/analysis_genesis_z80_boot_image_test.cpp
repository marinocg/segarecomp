// SEG-040-T004 (ADR 0072 section 4, report-only): the M68K-side Z80 boot-image producer (z80_boot_image.cpp) and its credited
// use by the report driver (report.cpp replacing its previous unconditional `z80_images = std::nullopt` /
// `held_in_reset = false`). Project-authored synthetic MC68000 (M68000PRM) and Zilog UM0080 fixtures only; no commercial input.
//
//   1. a fully-unrolled M68K boot sequence (BUSREQ assert, /RESET release, then byte-exact MOVE.B stores into the Z80 RAM
//      mirror) that writes the SAME 14-byte Zilog UM0080 program SEG-030-T010's own `proven_free()` fixture uses: the derived
//      image is credited and the Z80-side proof bounds the write set (`none`, same as the pre-existing direct-image test);
//   2. fail-closed: one boot-window byte comes from an Unknown-valued register (loaded through an Unknown address register):
//      no image is derived; the run stays on the blanket `image_set_unknown` rule, exactly as before SEG-040-T004;
//   3. layered fail-closed: even a fully M68K-exact derived image whose Z80-side content itself has a genuinely unknown
//      target ((HL) with an Unknown HL, SEG-030-T010's own `unknown_hl` fixture) stays `all` (`store_target_unknown`): the
//      Z80-side proof's own fail-closed guarantees are unaffected by this producer;
//   4. a Z80-RAM-mirror READ during the proven boot window never promotes to a write: byte-identical output to a program with
//      no Z80 access at all;
//   5. a device/control-register write (the bank register, $A06000, inside the broader Z80 bus area but outside the Z80 RAM
//      mirror) during the boot window, even with an Unknown value, never corrupts or disqualifies the derived RAM image.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "segarecomp/genesis_analysis_report/report.hpp"

namespace {

using namespace segarecomp;

int failures = 0;
void expect(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

// A tiny MC68000 assembler (M68000PRM MOVE encodings only; opcodes cross-checked against the existing
// analysis_genesis_z80_proof_test.cpp / analysis_m68k_equivalence_test.cpp fixtures).
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
  M68k &move_w_abs(std::uint16_t imm, std::uint32_t address) { return w(0x33FCU).w(imm).l(address); }
  M68k &move_b_abs(std::uint8_t imm, std::uint32_t address) { return w(0x13FCU).w(imm).l(address); }
  M68k &move_b_reg_to_abs(unsigned reg, std::uint32_t address) { return w(0x13C0U | reg).l(address); }
  M68k &move_b_ind_to_reg(unsigned dst, unsigned src_areg) { return w(0x1010U | dst << 9U | src_areg); }
  M68k &move_b_abs_to_reg(unsigned dst, std::uint32_t address) { return w(0x1039U | dst << 9U).l(address); }
  M68k &bra_self() { return w(0x60FEU); }
};

// SEG-030-T010's own 14-byte Zilog UM0080 `proven_free()` fixture (ld sp,$1FF0; ld a,$2A; ld ($1000),a; ld hl,$1100;
// ld (hl),a; jr $-2): a Z80 program the existing proof already proves `none` when given directly as an image. Reused
// unmodified here so that the M68K-derivation test asserts the same, already-established Z80-side outcome.
const std::vector<std::uint8_t> &proven_free_z80_bytes() {
  static const std::vector<std::uint8_t> bytes = {0x31U, 0xF0U, 0x1FU, 0x3EU, 0x2AU, 0x32U, 0x00U, 0x10U,
                                                   0x21U, 0x00U, 0x11U, 0x77U, 0x18U, 0xFEU};
  return bytes;
}

// SEG-030-T010's own `unknown_hl` fixture (ld sp,$1FF0; ld a,(1000); ld h,a; ld (hl),a; jr $-2): the Z80 proof's own
// fail-closed `store_target_unknown` (an Unknown HL).
const std::vector<std::uint8_t> &unknown_hl_z80_bytes() {
  static const std::vector<std::uint8_t> bytes = {0x31U, 0xF0U, 0x1FU, 0x3AU, 0x00U, 0x10U, 0x67U, 0x77U, 0x18U, 0xFEU};
  return bytes;
}

constexpr std::uint32_t z80_busreq = 0x00A11100U;
constexpr std::uint32_t z80_reset = 0x00A11200U;
constexpr std::uint32_t z80_ram = 0x00A00000U;
constexpr std::uint32_t z80_bank_register = 0x00A06000U;

std::optional<FrontendProgram> build(const M68k &m) {
  auto program = make_genesis_bridge_startup_program(m.bytes, 0U, 0x200U, std::nullopt);
  if (!program || !apply_genesis_immutable_rom_aot(*program)) return std::nullopt;
  return program;
}

GenesisAnalysisReportConfig memory_config() {
  GenesisAnalysisReportConfig config{};
  config.domains.memory = true;
  config.reset_entry = true;
  return config;
}

// SEG-030-T010's own proof vocabulary (z80_ram_write_proof.hpp), reused to describe expectations precisely.
bool has(const GenesisZ80RamWriteProof &proof, GenesisZ80ProofReason reason) { return proof.reasons.contains(reason); }

// 1. A fully M68K-exact boot upload: the derived image is credited and the Z80-side proof bounds the write set.
void exact_upload_credited() {
  M68k m;
  m.move_w_abs(0x0100U, z80_busreq);  // BUSREQ assert (request the bus)
  m.move_w_abs(0x0100U, z80_reset);   // /RESET release (now bus-granted: reset_released && busreq -- the pristine boot window)
  const auto &z80 = proven_free_z80_bytes();
  for (std::size_t i = 0; i < z80.size(); ++i) m.move_b_abs(z80[i], z80_ram + static_cast<std::uint32_t>(i));
  m.move_w_abs(0x0000U, z80_busreq);  // BUSREQ release (the Z80 may now run)
  m.bra_self();
  const auto program = build(m);
  expect(program.has_value(), "exact_upload_credited: program");
  if (!program) return;
  const auto report = run_genesis_analysis_report(*program, memory_config());
  expect(report.analysis.complete, "exact_upload_credited: analysis completes");
  expect(report.z80_proof.has_value(), "exact_upload_credited: a proof is produced");
  if (!report.z80_proof) return;
  expect(report.z80_proof->outcome == GenesisZ80RamWrites::none && report.z80_proof->reasons.empty() &&
             report.z80_proof->image_hashes.size() == 1U,
         "exact_upload_credited: the M68K-derived image reproduces the direct-image `none` outcome (" +
             std::string(genesis_z80_ram_writes_name(report.z80_proof->outcome)) + ")");
  expect(report.z80_bound_credited, "exact_upload_credited: the credited (not blanket) bound is used");
}

// 2. Fail-closed: one boot-window byte is Unknown (read through an Unknown address register). No image is derived.
void unknown_value_fails_closed() {
  M68k m;
  m.move_w_abs(0x0100U, z80_busreq);
  m.move_w_abs(0x0100U, z80_reset);
  m.move_b_abs(0x31U, z80_ram + 0U);        // offset 0: exact
  m.move_b_ind_to_reg(0U, 0U);              // D0 <- (A0); A0 is never written: Unknown
  m.move_b_reg_to_abs(0U, z80_ram + 1U);    // offset 1: Unknown value
  m.move_b_abs(0x1FU, z80_ram + 2U);        // offset 2: exact (never reached: the image is already disqualified)
  m.bra_self();
  const auto program = build(m);
  expect(program.has_value(), "unknown_value_fails_closed: program");
  if (!program) return;
  const auto report = run_genesis_analysis_report(*program, memory_config());
  expect(report.analysis.complete && report.z80_proof.has_value(), "unknown_value_fails_closed: a proof is produced");
  if (!report.z80_proof) return;
  expect(report.z80_proof->outcome == GenesisZ80RamWrites::all && has(*report.z80_proof, GenesisZ80ProofReason::image_set_unknown) &&
             !report.z80_bound_credited,
         "unknown_value_fails_closed: one Unknown boot-window byte abandons the whole image (never a partially-known one): " +
             std::string(genesis_z80_ram_writes_name(report.z80_proof->outcome)));
}

// 3. Layered fail-closed: an M68K-exact derived image whose Z80-side content is itself genuinely unbounded stays `all`.
void z80_side_unknown_preserved() {
  M68k m;
  m.move_w_abs(0x0100U, z80_busreq);
  m.move_w_abs(0x0100U, z80_reset);
  const auto &z80 = unknown_hl_z80_bytes();
  for (std::size_t i = 0; i < z80.size(); ++i) m.move_b_abs(z80[i], z80_ram + static_cast<std::uint32_t>(i));
  m.bra_self();
  const auto program = build(m);
  expect(program.has_value(), "z80_side_unknown_preserved: program");
  if (!program) return;
  const auto report = run_genesis_analysis_report(*program, memory_config());
  expect(report.analysis.complete && report.z80_proof.has_value(), "z80_side_unknown_preserved: a proof is produced");
  if (!report.z80_proof) return;
  expect(report.z80_proof->outcome == GenesisZ80RamWrites::all &&
             has(*report.z80_proof, GenesisZ80ProofReason::store_target_unknown) && report.z80_proof->image_hashes.size() == 1U,
         "z80_side_unknown_preserved: a fully M68K-exact image whose own Z80 content is unbounded still fails closed (" +
             std::string(genesis_z80_ram_writes_name(report.z80_proof->outcome)) + ")");
}

// 4. A Z80-RAM-mirror READ during the proven boot window never promotes to a write.
void read_never_promotes_to_write() {
  M68k with_read;
  with_read.move_w_abs(0x0100U, z80_busreq);
  with_read.move_w_abs(0x0100U, z80_reset);
  with_read.move_b_abs_to_reg(0U, z80_ram + 5U);  // a pure load: no memory write at all
  with_read.bra_self();
  M68k without_read;
  without_read.move_w_abs(0x0100U, z80_busreq);
  without_read.move_w_abs(0x0100U, z80_reset);
  without_read.bra_self();
  const auto program_with = build(with_read);
  const auto program_without = build(without_read);
  expect(program_with.has_value() && program_without.has_value(), "read_never_promotes_to_write: programs");
  if (!program_with || !program_without) return;
  const auto config = memory_config();
  const auto report_with = run_genesis_analysis_report(*program_with, config);
  const auto report_without = run_genesis_analysis_report(*program_without, config);
  expect(report_with.z80_proof.has_value() && report_without.z80_proof.has_value(),
         "read_never_promotes_to_write: both proofs produced");
  if (!report_with.z80_proof || !report_without.z80_proof) return;
  expect(format_genesis_z80_ram_write_proof(*report_with.z80_proof, "test") ==
             format_genesis_z80_ram_write_proof(*report_without.z80_proof, "test"),
         "read_never_promotes_to_write: a Z80 RAM read changes nothing the proof reports");
}

// 5. A device/control-register write (the bank register) inside the Z80 bus area but outside the Z80 RAM mirror, even with
// an Unknown value, never corrupts or disqualifies the derived RAM image.
void device_register_write_does_not_corrupt_ram_image() {
  M68k m;
  m.move_w_abs(0x0100U, z80_busreq);
  m.move_w_abs(0x0100U, z80_reset);
  m.move_b_abs(0x31U, z80_ram + 0U);                 // the one Z80 RAM byte of interest: exact
  m.move_b_ind_to_reg(0U, 0U);                       // D0 <- (A0); A0 is Unknown
  m.move_b_reg_to_abs(0U, z80_bank_register);        // the bank register, NOT the Z80 RAM mirror: Unknown value
  m.bra_self();
  const auto program = build(m);
  expect(program.has_value(), "device_register_write_does_not_corrupt_ram_image: program");
  if (!program) return;
  const auto report = run_genesis_analysis_report(*program, memory_config());
  expect(report.analysis.complete && report.z80_proof.has_value(),
         "device_register_write_does_not_corrupt_ram_image: a proof is produced");
  if (!report.z80_proof) return;
  // A one-byte Z80 image of just $31 ($0x31) is undecodable from reset (LD SP,nn needs 3 bytes): the Z80-side proof itself
  // fails closed on it (`undecodable_code`), which is the expected, honest outcome of a deliberately truncated/minimal
  // fixture -- the point here is only that an image was derived at all (`image_hashes.size() == 1`), proving the device
  // write did not block derivation.
  expect(report.z80_proof->image_hashes.size() == 1U,
         "device_register_write_does_not_corrupt_ram_image: the device-register write did not disqualify the RAM image");
}

// 6. SEG-040-T007 adversarial finding: a reachable interrupt-handler partition's own store into the Z80 RAM mirror is a real
// 68K write this producer's main-flow-only (tag 0) replay never visits. The pre-existing, all-partition
// `observed_store_ranges` aggregate (SEG-030-T010) already saw such a store; this producer must not silently discard that fact
// merely because it also found a main-flow write of its own (report.cpp unconditionally *replaces*, rather than unions, the
// all-partition aggregate with `boot.area_stores` whenever the latter is non-empty).
void handler_partition_store_is_not_dropped() {
  M68k m;
  m.move_w_abs(0x0100U, z80_busreq);
  m.move_w_abs(0x0100U, z80_reset);
  const auto &z80 = proven_free_z80_bytes();
  for (std::size_t i = 0; i < z80.size(); ++i) m.move_b_abs(z80[i], z80_ram + static_cast<std::uint32_t>(i));
  m.move_w_abs(0x0000U, z80_busreq);
  m.bra_self();
  // Install a delivered interrupt vector (level 6, vector 30: the Genesis VBlank source) whose handler also stores into the
  // Z80 RAM mirror, overwriting one of the uploaded bytes. The vector table slot makes the handler a reachable root
  // (SEG-030-T006) regardless of whether main flow ever raises the interrupt mask.
  constexpr std::uint32_t handler_pc = 0x300U;
  m.put32(30U * 4U, handler_pc);
  m.pc = handler_pc;
  m.move_b_abs(0x99U, z80_ram + 5U);  // conflicts with the uploaded program's own byte at offset 5
  m.w(0x4E73U);                       // RTE
  const auto program = build(m);
  expect(program.has_value(), "handler_partition_store_is_not_dropped: program");
  if (!program) return;
  auto config = memory_config();
  config.domains.frames = true;  // required for the handler to be analysed as its own partition
  const auto report = run_genesis_analysis_report(*program, config);
  expect(report.analysis.complete && report.z80_proof.has_value(), "handler_partition_store_is_not_dropped: a proof is produced");
  if (!report.z80_proof) return;
  expect(report.z80_proof->outcome != GenesisZ80RamWrites::none,
         "handler_partition_store_is_not_dropped: a handler-partition store into Z80 RAM must not be silently dropped (" +
             std::string(genesis_z80_ram_writes_name(report.z80_proof->outcome)) + ")");
}

}  // namespace

int main() {
  exact_upload_credited();
  unknown_value_fails_closed();
  z80_side_unknown_preserved();
  read_never_promotes_to_write();
  device_register_write_does_not_corrupt_ram_image();
  handler_partition_store_is_not_dropped();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cout << "analysis_genesis_z80_boot_image_test: all checks passed\n";
  return EXIT_SUCCESS;
}
