// SEG-028-T004 (ADR 0077): the Genesis M68K executable-image producer (genesis_m68k_executable_images) and the equivalence of the
// ADR 0049 alias identities derived from its `static_proof` images with the historical descriptor loop.
//
// `legacy_alias_identities` is the pre-SEG-028 alias loop of populate_immutable_rom_aot_entries
// (platforms/genesis/machine/src/frontend.cpp), re-implemented over the public decoder/lifter with the same file-local helpers
// (claim ownership by address, structural claim validity, the work-RAM window). Project-authored synthetic bytes only: the routine
// is the SEG-021-T041 immutable-copy alias fixture of tests/m68k_pipeline_test.cpp.
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "segarecomp/cpu/m68k/decode.hpp"
#include "segarecomp/cpu/m68k/ir.hpp"
#include "segarecomp/machine/genesis/frontend.hpp"

namespace {

using namespace segarecomp;

int failures = 0;

void check(bool ok, const std::string& label) {
  std::printf("%-5s %s\n", ok ? "ok" : "FAIL", label.c_str());
  if (!ok) ++failures;
}

constexpr std::uint32_t base = 0x00000E00U;
constexpr std::uint32_t source_offset = 0x40U;
constexpr std::uint32_t routine_length = 0x28U;
constexpr std::uint32_t execution_base = 0x00FF0400U;
constexpr std::uint32_t work_ram_begin = 0x00FF0000U;
constexpr std::uint32_t work_ram_end = 0x01000000U;

const std::vector<std::uint8_t> image{
    0x4EU, 0xB9U, 0x00U, 0x00U, 0x0EU, 0x0EU,  // +0x00 JSR $00000E0E
    0x30U, 0x51U,                              // +0x06 MOVEA.W (A1),A0
    0x4EU, 0x90U,                              // +0x08 JSR (A0)
    0x4EU, 0x71U,                              // +0x0A NOP
    0x60U, 0xF2U,                              // +0x0C BRA.S -> base
    0x4EU, 0x75U,                              // +0x0E RTS
    0x41U, 0xFAU, 0x00U, 0x2EU,                // +0x10 LEA (d16,PC),A0
    0x43U, 0xF9U, 0x00U, 0xFFU, 0x04U, 0x00U,  // +0x14 LEA $00FF0400.L,A1
    0x74U, 0x09U,                              // +0x1A MOVEQ #9,D2
    0x22U, 0xD8U,                              // +0x1C MOVE.L (A0)+,(A1)+
    0x51U, 0xCAU, 0xFFU, 0xFCU,                // +0x1E DBRA D2,-4
    0x4EU, 0xB9U, 0x00U, 0xFFU, 0x04U, 0x00U,  // +0x22 JSR $00FF0400.L
    0x4EU, 0x71U,                              // +0x28 NOP
    0x60U, 0xFEU,                              // +0x2A BRA.S *
    0x4EU, 0x71U, 0x4EU, 0x71U, 0x4EU, 0x71U, 0x4EU, 0x71U, 0x4EU, 0x71U,
    0x4EU, 0x71U, 0x4EU, 0x71U, 0x4EU, 0x71U, 0x4EU, 0x71U, 0x4EU, 0x71U,
    // routine (source +0x40, executes at $00FF0400)
    0x70U, 0x05U,                              // MOVEQ #5,D0
    0x61U, 0x18U,                              // BSR.S
    0x4AU, 0x40U,                              // TST.W D0
    0x66U, 0x04U,                              // BNE.S
    0x7CU, 0x63U,                              // MOVEQ #$63,D6
    0x4EU, 0x71U,                              // NOP
    0x30U, 0x3AU, 0x00U, 0x16U,                // MOVE.W (d16,PC),D0
    0x33U, 0xC0U, 0x00U, 0xFFU, 0x08U, 0x00U,  // MOVE.W D0,$00FF0800.L
    0x4EU, 0x75U,                              // RTS
    0x4EU, 0x71U, 0x4EU, 0x71U,                // NOP NOP
    0x52U, 0x40U,                              // ADDQ.W #1,D0
    0x4EU, 0x75U,                              // RTS
    0x4EU, 0x71U, 0x4EU, 0x71U,                // NOP NOP
    0x12U, 0x34U, 0x00U, 0x00U,                // data
};

MappingClaim claim(const char* name, std::uint32_t target_begin, std::uint32_t target_end, std::uint64_t image_begin,
                   std::uint64_t image_end) {
  return {name, {TargetAddressSpace::m68k_program, target_begin}, {TargetAddressSpace::m68k_program, target_end}, {image_begin},
          {image_end}};
}

FrontendProgram program_with() {
  FrontendProgram program{};
  program.profile = M68kFrontendProfile::general_startup;
  program.image = {"synthetic/SEG-028-T004/immutable-copy-alias", image, image.size()};
  program.mapping_claims = {claim("raw_cartridge_rom", base, static_cast<std::uint32_t>(base + image.size()), 0U, image.size())};
  program.startup_ingress = M68kStartupIngress{{{}, base}, 0x00FF1000U};
  return program;
}

// ---- the historical alias loop ---------------------------------------------------------------------------------------------------

std::vector<const MappingClaim*> legacy_claims(const std::vector<MappingClaim>& mappings, std::uint32_t pc) {
  std::vector<const MappingClaim*> out;
  for (const auto& m : mappings)
    if (pc >= m.target_begin.value && pc < m.target_end.value) out.push_back(&m);
  return out;
}

bool legacy_structurally_valid(const MappingClaim& claim) {
  return claim.target_begin.space == TargetAddressSpace::m68k_program && claim.target_end.space == TargetAddressSpace::m68k_program &&
         claim.target_begin.value < claim.target_end.value && claim.image_begin.value < claim.image_end.value &&
         static_cast<std::uint64_t>(claim.target_end.value) - claim.target_begin.value == claim.image_end.value - claim.image_begin.value;
}

struct Identity {
  std::vector<std::uint8_t> raw_bytes;
  std::uint32_t address = 0;
  std::uint64_t image_offset = 0;
  std::uint64_t length = 0;
  M68kInstructionKind kind{};
  M68kIrKind ir{};
  std::uint32_t alias_source_address = 0;
  std::string claim_name;
  std::uint32_t claim_begin = 0;
  std::uint32_t claim_end = 0;
  friend bool operator==(const Identity&, const Identity&) = default;
};

Identity identity_of(const M68kDecodedInstruction& decoded, const M68kIrOperation& operation, const MappingClaim& claim,
                     std::uint32_t alias_source) {
  return {decoded.raw_bytes,          decoded.provenance.source.address.value, decoded.provenance.source.image_offset.value,
          decoded.provenance.length.value, decoded.kind,                       operation.kind,
          alias_source,               claim.name,                              claim.target_begin.value,
          claim.target_end.value};
}

// Pre-SEG-028 body (alias part only; the equality of a re-inserted identity is checked through raw bytes and kinds).
std::optional<std::map<std::uint32_t, Identity>> legacy_alias_identities(const FrontendProgram& program,
                                                                          bool return_target_authority_available) {
  std::map<std::uint32_t, Identity> entries;
  for (const auto& alias : program.immutable_copy_aliases) {
    const auto owners = legacy_claims(program.mapping_claims, alias.source_base);
    if (owners.size() != 1U || owners.front()->name != "raw_cartridge_rom" || !legacy_structurally_valid(*owners.front()))
      return std::nullopt;
    const auto& claim = *owners.front();
    const std::uint64_t source_end = static_cast<std::uint64_t>(alias.source_base) + alias.length;
    if (alias.execution_base < work_ram_begin || static_cast<std::uint64_t>(alias.execution_base) + alias.length > work_ram_end ||
        source_end > claim.target_end.value || claim.image_end.value > program.image.bytes.size())
      return std::nullopt;
    const auto claim_size = claim.image_end.value - claim.image_begin.value;
    const auto claim_bytes = std::span<const std::uint8_t>(program.image.bytes)
                                 .subspan(static_cast<std::size_t>(claim.image_begin.value), static_cast<std::size_t>(claim_size));
    for (std::uint32_t offset = 0U; offset < alias.length; offset += 2U) {
      const std::uint32_t execution = alias.execution_base + offset;
      const std::uint64_t local = static_cast<std::uint64_t>(alias.source_base + offset) - claim.target_begin.value;
      const std::uint64_t image_offset = claim.image_begin.value + local;
      DecodeSource source{CpuVariant::mc68000, {TargetAddressSpace::m68k_program, execution}, MoveqImageOffset{local}};
      auto decoded_result = decode_m68k_instruction(claim_bytes, source, M68kDecodeProfile::general_startup);
      auto* decoded = std::get_if<M68kDecodedInstruction>(&decoded_result);
      if (decoded == nullptr) continue;
      decoded->provenance.source.image_offset = MoveqImageOffset{image_offset};
      const std::uint64_t length = decoded->provenance.length.value;
      if (length < 2U || offset + length > alias.length) continue;
      auto operation = lift_m68k_instruction(*decoded);
      if (!m68k_operation_is_immutable_rom_aot_safe(operation, return_target_authority_available)) continue;
      const Identity candidate = identity_of(*decoded, operation, claim, alias.source_base + offset);
      const auto [found, inserted] = entries.emplace(execution, candidate);
      if (!inserted && !(found->second == candidate)) return std::nullopt;
    }
  }
  return entries;
}

// The alias identities the current analysis derives (from the executable images), keyed by execution address.
struct Analysis {
  bool ok = false;
  bool return_authority = false;
  std::map<std::uint32_t, Identity> aliases;
  std::size_t non_alias = 0;
};

Analysis analyze(const FrontendProgram& program) {
  Analysis out;
  const auto result = analyze_m68k_frontend(program);
  const FrontendAnalysis* analysis = nullptr;
  if (const auto* partial = std::get_if<FrontendPartialProgram>(&result)) analysis = &partial->accepted_prefix;
  else if (const auto* full = std::get_if<FrontendAnalysis>(&result)) analysis = full;
  if (analysis == nullptr) return out;
  out.ok = true;
  out.return_authority = !analysis->static_frames.empty();
  for (const auto& entry : analysis->immutable_rom_aot_entries) {
    if (!entry.execution_alias) {
      ++out.non_alias;
      continue;
    }
    out.aliases.emplace(entry.decoded.provenance.source.address.value,
                        identity_of(entry.decoded, entry.operation, entry.source_mapping, entry.alias_source_address));
  }
  return out;
}

// ---- producer shape ----------------------------------------------------------------------------------------------------------------

void producer_shape() {
  {
    const auto program = program_with();
    const auto images = genesis_m68k_executable_images(program);
    check(images && images->set.cpu == CpuVariant::mc68000 && !images->set.platform_selector && images->set.images.size() == 1 &&
              images->claim_index == std::vector<std::size_t>{0},
          "no descriptor: one cartridge image");
    if (images) {
      const auto& cart = images->set.images[0];
      check(cart.id == ImageId{1} && cart.bytes == image && !cart.source && cart.mappings.size() == 1 &&
                cart.mappings[0] == ImageMapping{base, 0, static_cast<std::uint32_t>(image.size())} &&
                cart.provenance.authority == ImageAuthority::immutable_input && cart.provenance.producer == "genesis.cartridge" &&
                cart.verification == ImageVerification::none,
            "cartridge image: owned claim bytes at the claim base, immutable_input, genesis.cartridge, none");
    }
  }
  {
    auto program = program_with();
    check(apply_genesis_immutable_copy_alias(program, execution_base + 0x100U, base + source_offset, 0x10U) &&
              apply_genesis_immutable_copy_alias(program, execution_base, base + source_offset, routine_length),
          "two descriptors accepted (inserted out of execution order)");
    const auto images = genesis_m68k_executable_images(program);
    check(images && images->set.images.size() == 3 && images->claim_index == std::vector<std::size_t>{0, 0, 0} &&
              validate_executable_image_set(images->set).ok(),
          "two descriptors: cartridge + two alias images, all from claim 0");
    if (images && images->set.images.size() == 3) {
      const auto& a = images->set.images[1];
      const auto& b = images->set.images[2];
      check(a.id == ImageId{2} && a.bytes.empty() && a.source == ImageSourceReference{ImageId{1}, source_offset, routine_length} &&
                a.mappings == std::vector<ImageMapping>{{execution_base, 0, routine_length}} &&
                a.provenance.authority == ImageAuthority::static_proof && a.provenance.producer == "genesis.copy_alias" &&
                a.verification == ImageVerification::byte_identity,
            "alias image 2: descriptor order (execution-sorted), source reference, work-RAM mapping, static_proof, byte_identity");
      check(b.id == ImageId{3} && b.source == ImageSourceReference{ImageId{1}, source_offset, 0x10U} &&
                b.mappings == std::vector<ImageMapping>{{execution_base + 0x100U, 0, 0x10U}},
            "alias image 3: second descriptor");
      const auto bytes = executable_image_bytes(images->set, a);
      check(std::vector<std::uint8_t>(bytes.begin(), bytes.end()) ==
                std::vector<std::uint8_t>(image.begin() + source_offset, image.begin() + source_offset + routine_length),
            "alias bytes resolve to the verbatim source routine");
      const auto counts = format_image_provenance_json(count_image_provenance(images->set));
      check(counts ==
                "{\"images\":3,\"authority\":{\"immutable_input\":1,\"static_proof\":2,\"bounded_build_time_materialization\":0},"
                "\"producers\":{\"genesis.cartridge\":1,\"genesis.copy_alias\":2}}",
            "sanitized provenance: " + counts);
    }
  }
  {  // several claims: only valid raw cartridge claims become images; an alias references its own claim's image
    FrontendProgram program = program_with();
    const auto half = static_cast<std::uint32_t>(0x30U);
    program.mapping_claims = {
        claim("raw_cartridge_rom", base, base + half, 0U, half),
        claim("work_ram", 0x00FF0000U, 0x00FF0100U, 0U, 0x100U),                     // not a cartridge claim
        claim("raw_cartridge_rom", 0x00100000U, 0x00100010U, 0U, 0x20U),              // structurally invalid (length mismatch)
        claim("raw_cartridge_rom", base + half, static_cast<std::uint32_t>(base + image.size()), half, image.size()),
    };
    program.immutable_copy_aliases.push_back({execution_base, base + source_offset, routine_length});
    const auto images = genesis_m68k_executable_images(program);
    check(images && images->set.images.size() == 3 && images->claim_index == std::vector<std::size_t>{0, 3, 3},
          "two valid raw claims become images 1 and 2; the alias references claim 3's image");
    if (images && images->set.images.size() == 3) {
      check(images->set.images[1].mappings[0] == ImageMapping{base + half, 0, static_cast<std::uint32_t>(image.size() - half)} &&
                images->set.images[2].source == ImageSourceReference{ImageId{2}, source_offset - half, routine_length},
            "the alias source offset is relative to its owning claim image");
    }
  }
}

// ---- ADR 0097 amendment: the materialized RAM-thunk producer ---------------------------------------------------------------------

void ram_thunk_producer() {
  const std::vector<std::uint8_t> jmp_long{0x4E, 0xF9, 0x00, 0x00, 0x02, 0x00};
  const std::vector<std::uint8_t> jmp_word{0x4E, 0xF8, 0x12, 0x34};
  auto program = program_with();
  check(apply_genesis_immutable_copy_alias(program, execution_base, base + source_offset, routine_length), "alias accepted");
  // recognition is the CPU decoder's: exactly one JMP (xxx).W/.L, trimmed to the decoded length
  const std::vector<std::uint8_t> longer{0x4E, 0xF9, 0x00, 0x00, 0x02, 0x00, 0x4E, 0x71};
  check(genesis_ram_jump_thunk_bytes(0x00FFFA7CU, longer) == jmp_long, "recognised: JMP (xxx).L, trimmed to the decoded length");
  check(genesis_ram_jump_thunk_bytes(0x00FFFA7CU, jmp_word) == jmp_word, "recognised: JMP (xxx).W");
  check(!genesis_ram_jump_thunk_bytes(0x00FFFA7CU, std::vector<std::uint8_t>{0x4E, 0xB9, 0, 0, 2, 0}), "refused: JSR");
  check(!genesis_ram_jump_thunk_bytes(0x00FFFA7CU, std::vector<std::uint8_t>{0x4E, 0xD0, 0x4E, 0x71, 0, 0}), "refused: JMP (An)");
  check(!genesis_ram_jump_thunk_bytes(0x00FFFA7CU, std::vector<std::uint8_t>{0x4E, 0x71, 0x4E, 0x71, 0, 0}), "refused: NOP");
  check(!genesis_ram_jump_thunk_bytes(0x00FFFA7DU, jmp_long), "refused: odd base");
  check(!genesis_ram_jump_thunk_bytes(0x00000400U, jmp_long), "refused: not work RAM");
  check(!genesis_ram_jump_thunk_bytes(0x00FFFA7CU, std::vector<std::uint8_t>{0x4E, 0xF9, 0x00}), "refused: truncated");
  check(apply_genesis_materialized_ram_thunk(program, 0x00FFFA7CU, longer) &&
            apply_genesis_materialized_ram_thunk(program, 0x00FFFA7CU, jmp_long) /* exact duplicate */ &&
            apply_genesis_materialized_ram_thunk(program, 0x00FFFAD6U, jmp_word),
        "two thunks accepted (duplicate tolerated)");
  check(!apply_genesis_materialized_ram_thunk(program, 0x00FFFA7CU, jmp_word), "a different thunk over the same bytes is refused");
  check(!apply_genesis_materialized_ram_thunk(program, execution_base + 2U, jmp_long), "a thunk overlapping an alias is refused");
  check(program.materialized_ram_thunks.size() == 2 && program.materialized_ram_thunks[0].execution_base == 0x00FFFA7CU, "sorted descriptors");
  const auto images = genesis_m68k_executable_images(program);
  check(images && images->set.images.size() == 4 && validate_executable_image_set(images->set).ok(), "cartridge + alias + two thunk images validate");
  if (images && images->set.images.size() == 4) {
    const auto& t = images->set.images[2];
    check(t.id == ImageId{3} && t.bytes == jmp_long && !t.source && t.mappings == std::vector<ImageMapping>{{0x00FFFA7CU, 0, 6}} &&
              t.provenance.authority == ImageAuthority::bounded_build_time_materialization && t.provenance.producer == "genesis.ram_thunk" &&
              t.verification == ImageVerification::byte_identity,
          "thunk image: owned exact bytes, work-RAM mapping, bounded_build_time_materialization (never static_proof), byte_identity");
    const auto counts = format_image_provenance_json(count_image_provenance(images->set));
    check(counts ==
              "{\"images\":4,\"authority\":{\"immutable_input\":1,\"static_proof\":1,\"bounded_build_time_materialization\":2},"
              "\"producers\":{\"genesis.cartridge\":1,\"genesis.copy_alias\":1,\"genesis.ram_thunk\":2}}",
          "sanitized provenance: " + counts);
  }
  // Injected descriptors are re-validated by the decoder: a forged one is refused.
  auto forged = program_with();
  forged.materialized_ram_thunks.push_back({0x00FFFA7CU, std::vector<std::uint8_t>{0x4E, 0x71, 0x4E, 0x71}});
  check(!genesis_m68k_executable_images(forged).has_value(), "nullopt: a forged non-JMP thunk descriptor");
  // The AOT identity of a thunk carries the guard: derived by the CPU lifter, flagged as a materialized thunk.
  auto analysis_program = program_with();
  check(apply_genesis_immutable_rom_aot(analysis_program) && apply_genesis_materialized_ram_thunk(analysis_program, 0x00FFFA7CU, jmp_long),
        "thunk on top of the immutable-ROM AOT program");
}

// ---- fail-closed descriptor conditions (descriptors injected directly, bypassing the descriptor validator) ------------------------

void fail_closed() {
  const auto refused = [](FrontendProgram program, FrontendProgram::ImmutableCopyAlias alias, const std::string& label) {
    program.immutable_copy_aliases.push_back(alias);
    check(!genesis_m68k_executable_images(program).has_value(), "nullopt: " + label);
  };
  const std::uint32_t src = base + source_offset;
  refused(program_with(), {execution_base, 0x00001000U, routine_length}, "source in no claim");
  refused(program_with(), {0x00FEFFF0U, src, routine_length}, "execution below work RAM");
  refused(program_with(), {0x00FFFFF0U, src, routine_length}, "execution past the end of work RAM");
  refused(program_with(), {0x00000400U, src, routine_length}, "execution in cartridge space");
  refused(program_with(), {execution_base, src, static_cast<std::uint32_t>(image.size())}, "source past the claim end");
  {
    auto program = program_with();
    program.mapping_claims[0].name = "work_ram";
    refused(program, {execution_base, src, routine_length}, "source owned by a non-cartridge claim");
  }
  {
    auto program = program_with();
    program.mapping_claims.push_back(claim("raw_cartridge_rom", base, static_cast<std::uint32_t>(base + image.size()), 0U, image.size()));
    refused(program, {execution_base, src, routine_length}, "source owned by two claims");
  }
  {
    auto program = program_with();
    program.mapping_claims[0].image_end = MoveqImageOffset{image.size() + 2U};
    program.mapping_claims[0].target_end.value += 2U;
    refused(program, {execution_base, src, routine_length}, "claim image end beyond the input bytes");
  }
  {
    auto program = program_with();
    program.mapping_claims[0].target_end.value += 2U;  // structurally invalid: target and image lengths differ
    refused(program, {execution_base, src, routine_length}, "structurally invalid owning claim");
  }
  {  // overlapping execution spans of two descriptors (the descriptor validator refuses them; the image set does too)
    auto program = program_with();
    program.immutable_copy_aliases.push_back({execution_base, src, routine_length});
    refused(program, {execution_base + 0x10U, src, 0x10U}, "overlapping alias execution spans");
  }
  {  // a zero-length descriptor (refused by the descriptor validator) is an empty source reference
    refused(program_with(), {execution_base, src, 0U}, "zero-length descriptor");
  }
  {  // stricter than the historical loop: two overlapping raw cartridge claims make the whole set ambiguous, even when the alias
     // source is owned by exactly one of them (production builds one claim; only a hand-built claim list can reach this)
    auto program = program_with();
    program.mapping_claims.push_back(claim("raw_cartridge_rom", base, base + 0x10U, 0U, 0x10U));
    refused(program, {execution_base, src, routine_length}, "overlapping raw cartridge claims");
  }
}

// ---- alias identity equivalence with the historical loop ---------------------------------------------------------------------------

void identity_equivalence() {
  const auto compare = [](FrontendProgram program, const std::string& label) {
    const Analysis now = analyze(program);
    check(now.ok, label + ": analysis accepted");
    const auto legacy = legacy_alias_identities(program, now.return_authority);
    check(legacy.has_value(), label + ": historical loop accepts");
    if (!now.ok || !legacy) return;
    check(!legacy->empty() || program.immutable_copy_aliases.empty(), label + ": historical loop yields identities");
    check(now.aliases == *legacy, label + ": alias identities identical to the historical loop (" + std::to_string(legacy->size()) +
                                      " identities)");
  };
  {
    auto program = program_with();
    (void)apply_genesis_immutable_rom_aot(program);
    (void)apply_genesis_immutable_copy_alias(program, execution_base, base + source_offset, routine_length);
    compare(program, "fixture descriptor");
  }
  {
    auto program = program_with();
    (void)apply_genesis_immutable_rom_aot(program);
    (void)apply_genesis_immutable_copy_alias(program, execution_base, base + source_offset, routine_length);
    (void)apply_genesis_immutable_copy_alias(program, execution_base + 0x200U, base + 0x10U, 0x1CU);
    (void)apply_genesis_immutable_copy_alias(program, 0x00FFFFF4U, base + source_offset + 0x0CU, 0x0CU);
    compare(program, "three descriptors incl. one ending at the work-RAM top");
  }
  {
    auto program = program_with();
    (void)apply_genesis_immutable_rom_aot(program);
    compare(program, "no descriptor");
  }
  {  // the overall analysis fails closed exactly where a descriptor is refused
    auto program = program_with();
    (void)apply_genesis_immutable_rom_aot(program);
    program.immutable_copy_aliases.push_back({execution_base, 0x00001000U, routine_length});
    const auto result = analyze_m68k_frontend(program);
    const bool accepted = std::holds_alternative<FrontendPartialProgram>(result) || std::holds_alternative<FrontendAnalysis>(result);
    bool any_alias = false;
    if (const auto* partial = std::get_if<FrontendPartialProgram>(&result))
      for (const auto& entry : partial->accepted_prefix.immutable_rom_aot_entries) any_alias = any_alias || entry.execution_alias;
    check(!legacy_alias_identities(program, true).has_value() && !any_alias,
          std::string("unowned descriptor: no alias identity on either path (analysis ") + (accepted ? "accepted" : "rejected") + ")");
  }
}

}  // namespace

int main() {
  producer_shape();
  ram_thunk_producer();
  fail_closed();
  identity_equivalence();
  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("genesis_m68k_executable_image_test: all checks passed\n");
  return 0;
}
