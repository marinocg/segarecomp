#include "segarecomp/machine/genesis/frontend.hpp"
#include "segarecomp/machine/genesis/address_space.hpp"
#include "segarecomp/cpu/m68k/static_discovery.hpp"
#include "segarecomp/recompiler/frontend.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace segarecomp {
// CPU-owned proof-only helper used only by this aggregate owner.  It re-runs
// ADR-0009's existing finite-register lattice over a validated stitched decode
// graph; it cannot admit a target, create a static edge, or consume hints.
// SEG-007-T188 / ADR-0029: `canonical_edges` is the aggregated validated
// stitched control-edge relation (`merged_edges` as built up to this call).
// It is the sole post-stitch control-transfer reachability authority the
// re-run ADR-0009 lattice and its root selection consume.
[[nodiscard]] std::vector<M68kIndirectTargetEaSet> m68k_prove_stitched_an_indirect_targets(
    const std::map<std::uint32_t, M68kDecodedInstruction> &decoded_by_address,
    const std::vector<M68kProgramAddress> &entry_roots,
    const std::vector<M68kStaticEdge> &canonical_edges, std::uint32_t max_call_frame_depth,
    M68kStaticDiscoveryEnvironment &environment,
    const std::vector<M68kProgramAddress> &disconnected_root_candidates);
namespace {
std::optional<std::string_view> c4_stop_class(GenesisFrontierClass value) {
  switch (value) {
  case GenesisFrontierClass::unsupported_cpu_form: return "GENESIS_STOP_UNSUPPORTED_CPU_FORM";
  case GenesisFrontierClass::unsupported_device_access: return "GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS";
  case GenesisFrontierClass::unsupported_memory_region: return "GENESIS_STOP_UNSUPPORTED_MEMORY_REGION";
  case GenesisFrontierClass::unresolved_indirect_target: return "GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET";
  case GenesisFrontierClass::known_but_unemitted_target: return "GENESIS_STOP_KNOWN_BUT_UNEMITTED_TARGET";
  case GenesisFrontierClass::unsupported_interrupt_or_scheduling_event:
    return "GENESIS_STOP_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT";
  case GenesisFrontierClass::discovery_prefix_boundary: return "GENESIS_STOP_DISCOVERY_PREFIX_BOUNDARY";
  }
  return std::nullopt;
}
using Address = std::uint32_t;

std::string hex(std::uint64_t value, unsigned width) { std::ostringstream out; out << "0x" << std::uppercase << std::hex << std::setw(static_cast<int>(width)) << std::setfill('0') << value; return out.str(); }
bool same(const M68kProgramAddress &a, const M68kProgramAddress &b) { return a.space == b.space && a.value == b.value; }
bool less(const M68kProgramAddress &a, const M68kProgramAddress &b) { return a.space != b.space ? static_cast<unsigned>(a.space) < static_cast<unsigned>(b.space) : a.value < b.value; }
bool less(const BlockId &a, const BlockId &b) { return less(a.entry, b.entry); }
// SEG-007-T113 / ADR 0008: same single source of truth as the C11 emitter
// mirror and the generated-runtime provenance ABI (GENESIS_MAX_RAW_BYTES).
constexpr std::size_t genesis_frontier_max_raw_bytes = SEGARECOMP_GENESIS_MAX_RAW_INSTRUCTION_BYTES;
constexpr std::size_t genesis_frontier_max_name_length = 64U;
bool same_provenance(const InstructionProvenance &left, const InstructionProvenance &right) {
  return left.source.cpu_variant == right.source.cpu_variant && same(left.source.address, right.source.address) &&
         left.source.image_offset.value == right.source.image_offset.value && left.bytes == right.bytes &&
         left.length.value == right.length.value;
}
bool same_ea(const M68kEffectiveAddress &left, const M68kEffectiveAddress &right) {
  return left.mode == right.mode && left.reg == right.reg && left.displacement == right.displacement &&
         left.absolute_address == right.absolute_address && left.immediate_value == right.immediate_value &&
         left.extension_words == right.extension_words && left.index_reg == right.index_reg &&
         left.index_is_address == right.index_is_address && left.index_is_long == right.index_is_long &&
         left.pc_base_address == right.pc_base_address;
}
bool same_ir(const M68kIrOperation &left, const M68kIrOperation &right) {
  return same_provenance(left.provenance, right.provenance) && left.kind == right.kind &&
         left.destination == right.destination && left.operand == right.operand && left.raw_bytes == right.raw_bytes &&
         left.extension == right.extension && left.size == right.size && same_ea(left.source_ea, right.source_ea) &&
         same_ea(left.destination_ea, right.destination_ea) && left.condition == right.condition &&
         left.movem_direction == right.movem_direction && left.movem_register_mask == right.movem_register_mask &&
          left.shift_rotate_kind == right.shift_rotate_kind;
}
bool same_decoded(const M68kDecodedInstruction &left, const M68kDecodedInstruction &right) {
  return same_provenance(left.provenance, right.provenance) && left.kind == right.kind &&
         left.destination == right.destination && left.operand == right.operand &&
         left.raw_bytes == right.raw_bytes && left.extension == right.extension && left.size == right.size &&
         same_ea(left.source_ea, right.source_ea) && same_ea(left.destination_ea, right.destination_ea) &&
         left.condition == right.condition && left.movem_direction == right.movem_direction &&
         left.movem_register_mask == right.movem_register_mask && left.shift_rotate_kind == right.shift_rotate_kind;
}
bool same_call(const M68kStaticCall &left, const M68kStaticCall &right) {
  return same_provenance(left.caller, right.caller) && same(left.continuation, right.continuation) &&
         same(left.callee, right.callee);
}
bool valid_cpu_variant(CpuVariant value) { return value == CpuVariant::mc68000; }
bool valid_program_address(const M68kProgramAddress &value) {
  return value.space == TargetAddressSpace::m68k_program;
}
bool valid_access_width(M68kMemoryAccessWidth value) {
  return value == M68kMemoryAccessWidth::byte || value == M68kMemoryAccessWidth::word ||
         value == M68kMemoryAccessWidth::long_word;
}
bool valid_access_direction(M68kMemoryAccessDirection value) {
  return value == M68kMemoryAccessDirection::read || value == M68kMemoryAccessDirection::write;
}
// SEG-007-T032: JSON name vocabulary for the sanitized controller-I/O access
// shape projection. These names are the documented, stable serialized
// vocabulary (see the SEG-007-T032 backlog record Evidence); nothing
// else in the codebase names these values for JSON.
std::string_view m68k_memory_access_width_json_name(M68kMemoryAccessWidth value) {
  switch (value) {
  case M68kMemoryAccessWidth::byte: return "byte";
  case M68kMemoryAccessWidth::word: return "word";
  case M68kMemoryAccessWidth::long_word: return "long_word";
  }
  return "byte";
}
std::string_view m68k_memory_access_direction_json_name(M68kMemoryAccessDirection value) {
  return value == M68kMemoryAccessDirection::write ? "write" : "read";
}
std::string_view controller_io_access_shape_mismatch_json_name(ControllerIoAccessShapeMismatch value) {
  switch (value) {
  case ControllerIoAccessShapeMismatch::width: return "width";
  case ControllerIoAccessShapeMismatch::direction: return "direction";
  case ControllerIoAccessShapeMismatch::partial_or_crossing_shape: return "partial_or_crossing_shape";
  case ControllerIoAccessShapeMismatch::different_controller_io_register_shape:
    return "different_controller_io_register_shape";
  }
  return "";
}
// SEG-007-T035: JSON name vocabulary for the closed target-register-class
// tag. Lowercase snake_case, exactly matching the enum value names.
std::string_view controller_io_target_register_class_json_name(ControllerIoTargetRegisterClass value) {
  switch (value) {
  case ControllerIoTargetRegisterClass::version: return "version";
  case ControllerIoTargetRegisterClass::data: return "data";
  case ControllerIoTargetRegisterClass::ctrl: return "ctrl";
  case ControllerIoTargetRegisterClass::s_ctrl: return "s_ctrl";
  case ControllerIoTargetRegisterClass::tx_data: return "tx_data";
  case ControllerIoTargetRegisterClass::rx_data: return "rx_data";
  case ControllerIoTargetRegisterClass::unclassified: return "unclassified";
  }
  return "";
}
// SEG-007-T064 Checkpoint 7 / ADR 0004
// (docs/decisions/0004-generic-frontier-classification-privacy-boundary.md):
// the standard, publicly documented MC68000 "opcode line" grouping -- the
// top 4 bits of the instruction word, the exact 16-way grouping every public
// 68000 reference/disassembler already uses (Motorola's own M68000
// Programmer's Reference Manual opcode map). This is a fact about the
// 68000 ISA's own encoding structure, identical for any program in
// existence that happens to contain that bit pattern; it names no
// Sega-specific fact. It returns only the derived line name -- the
// instruction word's own raw bits, in any encoding, must never be printed,
// logged, or otherwise surfaced by any caller of this function.
std::string_view m68k_opcode_line_name(std::uint16_t word) noexcept {
  switch (static_cast<unsigned>((word >> 12U) & 0xFU)) {
  case 0x0U: return "bit_manipulation_movep_immediate";
  case 0x1U: return "move_byte";
  case 0x2U: return "move_long";
  case 0x3U: return "move_word";
  case 0x4U: return "miscellaneous";
  case 0x5U: return "addq_subq_scc_dbcc";
  case 0x6U: return "bcc_bsr_bra";
  case 0x7U: return "moveq";
  case 0x8U: return "or_div_sbcd";
  case 0x9U: return "sub_subx_suba";
  case 0xAU: return "reserved_line_a_emulator_trap";
  case 0xBU: return "cmp_eor_cmpa";
  case 0xCU: return "and_mul_abcd_exg";
  case 0xDU: return "add_addx_adda";
  case 0xEU: return "shift_rotate";
  default: return "reserved_line_f_emulator_trap";
  }
}
// SEG-007-T064 Checkpoint 7 / ADR 0004: reduces a target address (and its
// already-retained access width) to one of exactly four already-established,
// already-cited region buckets -- the same raw_cartridge_rom/
// synthetic_work_ram/hardware_frontier predicate the unconditional,
// ROM-free `probe-genesis-startup-mapping` CLI command already computes
// (raw_cartridge_rom is reduced here to "does address+width fit entirely
// inside a retained raw_cartridge_rom-named mapping claim", the identical
// [0, image_length) range that command's own inline check already tests,
// generalized to whatever target_begin/target_end that claim actually
// carries rather than re-deriving a separate image_length parameter), plus
// a fourth bucket for the already-cited GTO1 controller-I/O window
// (m68k_controller_io_region_begin..+0x20, the same comparison shape
// analyze_startup_profile's own synthetic-completion sentinel check already
// uses). Never a new, not-yet-independently-cited Z80-bus/VDP/other
// hardware address range. Returns only the derived bucket name -- the
// address value itself must never be printed, logged, or otherwise
// surfaced by any caller of this function.
std::string_view m68k_frontier_region_class_name(std::uint32_t address, std::uint32_t width,
                                                  const std::vector<MappingClaim> &mapping_claims) noexcept {
  for (const auto &claim : mapping_claims) {
    if (claim.name == "raw_cartridge_rom" && address >= claim.target_begin.value &&
        address < claim.target_end.value && width <= claim.target_end.value - address) {
      return "raw_cartridge_rom";
    }
  }
  if (m68k_startup_ram_range_in_range(address, width)) return "synthetic_work_ram";
  if (address >= m68k_controller_io_region_begin && address < m68k_controller_io_region_begin + 0x20U)
    return "controller_io_window";
  return "hardware_frontier";
}
bool structurally_valid_mapping_claim(const MappingClaim &claim) {
  return valid_program_address(claim.target_begin) && valid_program_address(claim.target_end) &&
         claim.target_begin.value < claim.target_end.value && claim.image_begin.value < claim.image_end.value &&
         static_cast<std::uint64_t>(claim.target_end.value) - claim.target_begin.value ==
             claim.image_end.value - claim.image_begin.value;
}

// A retained prefix has no image object from which a later emitter may recover
// ownership. This is the sole selector for its independently retained affine
// address/image mapping: image_begin + (address - target_begin) == offset.
const MappingClaim *select_unique_affine_mapping(const std::vector<MappingClaim> &claims,
                                                 const InstructionProvenance &provenance) {
  const auto address = provenance.source.address.value;
  const auto offset = provenance.source.image_offset.value;
  const auto length = provenance.length.value;
  if (!valid_cpu_variant(provenance.source.cpu_variant) || !valid_program_address(provenance.source.address) ||
      length < 2U || address > std::numeric_limits<Address>::max() - length ||
      offset > std::numeric_limits<std::uint64_t>::max() - length)
    return nullptr;

  // Validate every retained claim before mapping this span. A malformed
  // distractor is not harmless provenance that a later affine match may hide.
  for (const auto &claim : claims) {
    if (!structurally_valid_mapping_claim(claim)) return nullptr;
  }

  // Source ownership is decided before full-span and affine validation. A
  // second claim that owns the source byte is ambiguous even if it ends before
  // this instruction's verified span.
  const MappingClaim *selected = nullptr;
  for (const auto &claim : claims) {
    if (claim.target_begin.value > address || address >= claim.target_end.value) continue;
    if (selected != nullptr) return nullptr;
    selected = &claim;
  }

  if (selected == nullptr) return nullptr;
  if (address + length > selected->target_end.value) return nullptr;
  const auto delta = static_cast<std::uint64_t>(address - selected->target_begin.value);
  if (delta > std::numeric_limits<std::uint64_t>::max() - selected->image_begin.value) return nullptr;
  const auto mapped_offset = selected->image_begin.value + delta;
  if (mapped_offset != offset || selected->image_begin.value > offset || offset + length > selected->image_end.value)
    return nullptr;
  return selected;
}
bool independently_decoded_and_lifted(const M68kDecodedInstruction &instruction,
                                       const M68kIrOperation &operation) {
  auto raw_source = instruction.provenance.source;
  raw_source.image_offset = MoveqImageOffset{0U};
  auto result = decode_m68k_instruction(instruction.raw_bytes, raw_source, M68kDecodeProfile::general_startup);
  auto *redecoded = std::get_if<M68kDecodedInstruction>(&result);
  if (redecoded != nullptr) redecoded->provenance.source.image_offset = instruction.provenance.source.image_offset;
  return valid_cpu_variant(instruction.provenance.source.cpu_variant) &&
         valid_program_address(instruction.provenance.source.address) && instruction.provenance.length.value >= 2U &&
         instruction.raw_bytes.size() == instruction.provenance.length.value && instruction.raw_bytes.size() >= 2U &&
         instruction.raw_bytes[0] == instruction.provenance.bytes[0] &&
         instruction.raw_bytes[1] == instruction.provenance.bytes[1] && redecoded != nullptr &&
         same_decoded(instruction, *redecoded) && same_ir(operation, lift_m68k_instruction(instruction));
}
// SEG-007-T064: `sibling_frontier_addresses` is the same bounded set of
// allowed non-retained edge targets build_analysis used to decide which
// blocks to retain in `prefix` (this exit's own address is always a member).
// A single retained block may have outgoing edges to more than one retained
// exit (at most two, since a block's terminal instruction has at most two
// outgoing edges); each exit's own independent eligibility check must
// therefore recognize every sibling address build_analysis already treated
// as safe, not only its own -- otherwise a block build_analysis correctly
// retained could be rejected here for an edge this exact same call's own
// single-address view does not recognize, purely as an artifact of which
// exit happens to be validated first. This is the one behavioral extension
// beyond §1.4's known_but_unemitted_target arm and the `direct.has_target`
// exemption; every other clause below is reused unchanged, independently,
// for each exit.
//
// SEG-007-T183 / ADR-0028 §8: `semantic_partition_boundary` is a second,
// distinct allowed-edge-target set -- `FrontendAnalysis::semantic_partition_
// boundary_addresses`, threaded separately from `sibling_frontier_addresses`
// so the two invariants stay legible (one is the bounded diagnostic-frontier
// sibling set; the other is every validated static-partition destination
// independent of that report). A retained block's outgoing edge is safe when
// its target is a retained entry, this call's own frontier address, a
// sibling frontier address, OR a semantic partition boundary address.
bool runtime_frontier_eligible(const FrontendAnalysis &prefix, const FrontendRejected &diagnostic,
                                    GenesisFrontierClass frontier_class,
                                    const std::optional<M68kMemoryAccessRequest> &access,
                                    const std::set<Address> &sibling_frontier_addresses,
                                    const std::set<Address> &semantic_partition_boundary) {
  if (prefix.profile != M68kFrontendProfile::general_startup || prefix.static_blocks.empty() ||
      diagnostic.profile != M68kFrontendProfile::general_startup || diagnostic.supplied_image_source_id ||
      diagnostic.declared_image_byte_length || diagnostic.actual_image_byte_length ||
      diagnostic.available_bytes || diagnostic.requested_length || diagnostic.direct.has_instruction_length ||
      diagnostic.direct.available_bytes != 0U || diagnostic.direct.requested_length != 0U ||
      !diagnostic.direct.unresolved_reason.empty() || !diagnostic.source_address || !diagnostic.image_offset ||
      !diagnostic.provenance || !diagnostic.instruction_length || !diagnostic.direct.has_provenance ||
      diagnostic.direct.category != diagnostic.category ||
       !same(diagnostic.direct.block.entry, *diagnostic.source_address) ||
       !same_provenance(*diagnostic.provenance, diagnostic.direct.provenance) ||
       !valid_cpu_variant(diagnostic.provenance->source.cpu_variant) ||
       !valid_program_address(diagnostic.provenance->source.address) ||
       !same(*diagnostic.source_address, diagnostic.provenance->source.address) ||
        diagnostic.image_offset->value != diagnostic.provenance->source.image_offset.value ||
        *diagnostic.instruction_length != diagnostic.provenance->length.value)
    return false;
  // This is the sole C4 frontier-shape authority.  Construction and emission
  // both call it, so a hand-built partial cannot pair a plausible class with a
  // different diagnostic or silently acquire fields belonging to another
  // frontier class.
  const auto exact_category = [&]() {
    switch (frontier_class) {
    case GenesisFrontierClass::unsupported_cpu_form:
      return diagnostic.category == DirectFlowDiagnostic::valid_but_unsupported_instruction ||
             diagnostic.category == DirectFlowDiagnostic::unsupported_instruction_form;
    case GenesisFrontierClass::unsupported_device_access:
      return diagnostic.category == DirectFlowDiagnostic::unsupported_device_region_controller_io;
    case GenesisFrontierClass::unsupported_memory_region:
      return diagnostic.category == DirectFlowDiagnostic::unmapped_data_access;
    case GenesisFrontierClass::unresolved_indirect_target:
      return diagnostic.category == DirectFlowDiagnostic::reached_unresolved_direct_edge;
    // SEG-007-T064: known_but_unemitted_target's whole point is that it
    // accepts any underlying diagnostic.category -- the coarse "known but
    // unemitted" meaning is carried entirely by frontier_class itself, not
    // by a further category restriction. Every other structural check below
    // (provenance, mapping-claim/bus-access shape, reachability, ...) still
    // applies unchanged for this class.
    case GenesisFrontierClass::known_but_unemitted_target:
      return true;
    case GenesisFrontierClass::unsupported_interrupt_or_scheduling_event:
      return false; // C4 has no producer or lossless ABI lowering for this.
    // ADR 0013 Decision §6: the boundary's sole underlying diagnostic
    // category is discovery_budget_exhausted (Decision §3's empty-reason
    // discriminator already restricts classify_frontier's arm to that case).
    case GenesisFrontierClass::discovery_prefix_boundary:
      return diagnostic.category == DirectFlowDiagnostic::discovery_budget_exhausted;
    }
    return false;
  };
  if (!exact_category()) return false;
  if (frontier_class == GenesisFrontierClass::unresolved_indirect_target) {
    if (diagnostic.mapping_claims || !diagnostic.direct.mapping_claims.empty() || !diagnostic.accesses.empty() || access ||
        diagnostic.direct.has_target)
      return false;
  } else {
    if (!diagnostic.mapping_claims || diagnostic.mapping_claims->size() != 1U ||
        diagnostic.direct.mapping_claims.size() != diagnostic.mapping_claims->size()) return false;
    for (std::size_t index = 0; index < diagnostic.mapping_claims->size(); ++index) {
      const auto &claim = diagnostic.mapping_claims->at(index);
      const auto &direct_claim = diagnostic.direct.mapping_claims.at(index);
      if (claim.name.size() > genesis_frontier_max_name_length || claim.name != direct_claim.name ||
          !same(claim.target_begin, direct_claim.target_begin) || !same(claim.target_end, direct_claim.target_end) ||
          claim.image_begin.value != direct_claim.image_begin.value || claim.image_end.value != direct_claim.image_end.value)
        return false;
    }
    const auto length = diagnostic.provenance->length.value;
    const auto *claim = select_unique_affine_mapping(*diagnostic.mapping_claims, *diagnostic.provenance);
    if (claim == nullptr || claim != &diagnostic.mapping_claims->front() ||
        claim->name.size() > genesis_frontier_max_name_length || length > genesis_frontier_max_raw_bytes ||
        diagnostic.accesses.size() != 1U) return false;
    const auto &bus = diagnostic.accesses.front();
    if (bus.ordinal != 0U || bus.kind != StartupBusKind::instruction_read || bus.region != "raw_cartridge_rom" ||
        !same(bus.address, diagnostic.provenance->source.address) || !same_provenance(bus.instruction, *diagnostic.provenance) ||
        bus.bytes.size() != length || bus.bytes.size() < 2U || bus.bytes[0] != diagnostic.provenance->bytes[0] ||
        bus.bytes[1] != diagnostic.provenance->bytes[1]) return false;
  }
  if (frontier_class == GenesisFrontierClass::unsupported_cpu_form &&
      classify_m68k_cpu_frontier(*diagnostic.provenance) == M68kCpuFrontierKind::none)
    return false;
  const bool needs_access = frontier_class == GenesisFrontierClass::unsupported_device_access ||
                            frontier_class == GenesisFrontierClass::unsupported_memory_region;
  if (needs_access != access.has_value()) return false;
  if (access && (!valid_program_address(access->address) || !valid_access_width(access->width) ||
                 !valid_access_direction(access->direction) || !access->source_provenance ||
                 !same_provenance(*access->source_provenance, *diagnostic.provenance)))
    return false;
  if (needs_access && (!diagnostic.direct.has_target || !same(diagnostic.direct.target, access->address))) return false;
  // SEG-007-T064: known_but_unemitted_target is exempted from this guard.
  // Its own diagnostic may legitimately carry a known target address (for
  // example a sibling's own discovery_budget_exhausted rejection, which sets
  // direct.target/has_target via target_rejection) even though this class
  // needs no `access` request -- stripping direct.has_target from the reused
  // diagnostic would lose real host-side provenance instead.
  if (!needs_access && diagnostic.direct.has_target && frontier_class != GenesisFrontierClass::known_but_unemitted_target)
    return false;
  if (!prefix.startup_ingress || prefix.startup_ingress->entry.space != TargetAddressSpace::m68k_program) return false;
  std::map<Address, const M68kDecodedInstruction *> decoded_by_address;
  std::map<Address, const M68kIrOperation *> ir_by_address;
  for (const auto &decoded : prefix.decoded) {
    if (decoded.provenance.source.address.space != TargetAddressSpace::m68k_program ||
        !decoded_by_address.emplace(decoded.provenance.source.address.value, &decoded).second) return false;
  }
  if (prefix.decoded.size() != prefix.ir.size()) return false;
  for (std::size_t index = 0; index < prefix.decoded.size(); ++index) {
    if (!independently_decoded_and_lifted(prefix.decoded[index], prefix.ir[index]) ||
        !ir_by_address.emplace(prefix.ir[index].provenance.source.address.value, &prefix.ir[index]).second ||
        select_unique_affine_mapping(prefix.mapping_claims, prefix.decoded[index].provenance) == nullptr)
      return false;
  }

  std::set<Address> retained_entries;
  std::set<Address> retained_instructions;
  std::map<Address, const M68kStaticBlock *> blocks_by_entry;
  for (const auto &block : prefix.static_blocks) {
    if (block.instructions.empty() || block.id.entry.space != TargetAddressSpace::m68k_program ||
        block.instructions.front().source.address.value != block.id.entry.value ||
        !blocks_by_entry.emplace(block.id.entry.value, &block).second) return false;
    retained_entries.insert(block.id.entry.value);
    for (const auto &instruction : block.instructions) {
      const auto decoded = decoded_by_address.find(instruction.source.address.value);
      if (instruction.source.address.space != TargetAddressSpace::m68k_program || decoded == decoded_by_address.end() ||
          !same_provenance(instruction, decoded->second->provenance) ||
          !retained_instructions.insert(instruction.source.address.value).second) return false;
    }
  }
  if (retained_instructions.size() != prefix.decoded.size() ||
      !retained_entries.contains(prefix.startup_ingress->entry.value)) return false;
  const auto frontier_address = diagnostic.provenance->source.address.value;
  if (retained_instructions.contains(frontier_address)) return false;
  for (const auto &edge : prefix.static_edges) {
    const auto source = edge.source_instruction.source.address.value;
    const auto decoded = decoded_by_address.find(source);
    if (edge.source_instruction.source.address.space != TargetAddressSpace::m68k_program ||
        edge.target.space != TargetAddressSpace::m68k_program || decoded == decoded_by_address.end() ||
        !same_provenance(edge.source_instruction, decoded->second->provenance) ||
        (!retained_entries.contains(edge.target.value) && edge.target.value != frontier_address &&
         !sibling_frontier_addresses.contains(edge.target.value) &&
         !semantic_partition_boundary.contains(edge.target.value))) return false;
    const auto has_frame = [&](const M68kStaticCall &call) {
      return std::any_of(prefix.static_frames.begin(), prefix.static_frames.end(), [&](const M68kStaticFrame &frame) {
        return same_call(frame.call, call);
      });
    };
    switch (edge.kind) {
    case M68kStaticEdgeKind::direct_call:
      if (!edge.call || !same_provenance(edge.source_instruction, edge.call->caller) ||
          !same(edge.target, edge.call->callee) || !has_frame(*edge.call) ||
          (decoded->second->kind != M68kInstructionKind::jsr && decoded->second->kind != M68kInstructionKind::bsr))
        return false;
      break;
    case M68kStaticEdgeKind::return_to_continuation:
      // The return edge is sourced by the callee's RTS, not the caller that
      // opened its retained frame.  Its call identity binds the RTS to the
      // continuation without conflating those two instruction provenances.
      if (!edge.call || decoded->second->kind != M68kInstructionKind::rts ||
          !same(edge.target, edge.call->continuation) || !has_frame(*edge.call)) return false;
      break;
    case M68kStaticEdgeKind::fallthrough:
    // SEG-007-T181 / ADR-0027: the synthesized partition-boundary adjacency
    // edge -- validated exactly like a plain `fallthrough` (no call identity,
    // no branch/jump emitted; its target is kept a retained entry by the walk
    // below, ADR-0014 P1).
    case M68kStaticEdgeKind::fallthrough_continuation:
    case M68kStaticEdgeKind::direct_branch:
      if (edge.call) return false;
      break;
    case M68kStaticEdgeKind::indirect_call:
      // SEG-007-T124 / ADR-0009: one candidate member of a proven multi-
      // candidate JSR target set -- validated exactly like `direct_call`
      // above, except its target is one admitted candidate rather than the
      // single folded target.
      if (!edge.call || !same_provenance(edge.source_instruction, edge.call->caller) ||
          !same(edge.target, edge.call->callee) || !has_frame(*edge.call) ||
          decoded->second->kind != M68kInstructionKind::jsr)
        return false;
      break;
    case M68kStaticEdgeKind::indirect_branch:
      if (edge.call || decoded->second->kind != M68kInstructionKind::jmp) return false;
      break;
    }
  }
  for (const auto &frame : prefix.static_frames) {
    const auto caller = decoded_by_address.find(frame.call.caller.source.address.value);
    const auto continuation = frame.call.caller.source.address.value + frame.call.caller.length.value;
    const auto has_call_edge = std::any_of(prefix.static_edges.begin(), prefix.static_edges.end(), [&](const M68kStaticEdge &edge) {
      return (edge.kind == M68kStaticEdgeKind::direct_call || edge.kind == M68kStaticEdgeKind::indirect_call) &&
             edge.call && same_call(*edge.call, frame.call);
    });
    const auto frame_count = std::count_if(prefix.static_frames.begin(), prefix.static_frames.end(),
                                           [&](const M68kStaticFrame &other) {
      return same_call(other.call, frame.call);
    });
    // SEG-007-T134 correction (ADR-0011 Decision §1): this loop no longer
    // requires a `return_to_continuation` edge to be sourced at specifically
    // this frame's own callee-entry-block terminal instruction. That
    // narrower requirement was a leftover single-instruction-callee
    // assumption, not a soundness requirement: the per-edge switch above
    // already fully validates every `return_to_continuation` edge that IS
    // present (correct RTS instruction kind, target equal to exactly this
    // call's own continuation, and a uniquely matching retained frame).
    // Requiring an exact edge count keyed on "is the callee's OWN entry
    // block an RTS" wrongly rejected a whole-program-valid callee containing
    // an internal branch before its RTS, or exiting through more than one
    // RTS, even though `synthesize_return_edges` (static_discovery.cpp)
    // already represents every one of those legitimate exits correctly.
    if (caller == decoded_by_address.end() || !same_provenance(frame.call.caller, caller->second->provenance) ||
        (caller->second->kind != M68kInstructionKind::jsr && caller->second->kind != M68kInstructionKind::bsr) ||
        frame.call.continuation.space != TargetAddressSpace::m68k_program ||
        frame.call.callee.space != TargetAddressSpace::m68k_program || frame.call.continuation.value != continuation ||
        !has_call_edge || frame_count != 1U) return false;
  }
  // SEG-007-T242 / ADR-0039 ("successor freeze 1"): the final entry-rooted
  // static-knowledge reachability BFS that used to gate this function's
  // result on every retained block (and the frontier address itself) being
  // provably reachable from the known roots is removed. Missing static
  // graph reachability/ownership alone is no longer, by itself, sufficient
  // to reject an otherwise structurally valid, provenance-consistent
  // frontier candidate -- every check above (provenance/decode/lift
  // consistency, mapping-claim uniqueness, per-edge-kind structural
  // validity, and per-frame call/continuation consistency) is unchanged and
  // still fails closed.
  return true;
}
std::string json_string(std::string_view text) {
  // The generated dispatcher is derived only from this already validated static program.
  std::ostringstream out;
  out << '"';
  for (const char byte : text) {
    const auto character = static_cast<unsigned char>(byte);
    switch (character) {
    case '"': out << "\\\""; break;
    case '\\': out << "\\\\"; break;
    case '\n': out << "\\n"; break;
    case '\r': out << "\\r"; break;
    case '\t': out << "\\t"; break;
    case '\b': out << "\\b"; break;
    case '\f': out << "\\f"; break;
    default:
      if (character < 0x20U) {
        out << "\\u00" << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<unsigned>(character) << std::dec;
      } else {
        out << static_cast<char>(character);
      }
      break;
    }
  }
  return out << '"', out.str();
}
bool valid_claim(const MappingClaim &c, std::uint64_t size) {
  if (c.target_begin.space != TargetAddressSpace::m68k_program || c.target_end.space != TargetAddressSpace::m68k_program || c.target_begin.value >= c.target_end.value || c.image_begin.value >= c.image_end.value || c.image_end.value > size) return false;
  return static_cast<std::uint64_t>(c.target_end.value) - c.target_begin.value == c.image_end.value - c.image_begin.value;
}
std::vector<const MappingClaim *> claims(const std::vector<MappingClaim> &mappings, Address pc) {
  std::vector<const MappingClaim *> out; for (const auto &m : mappings) if (pc >= m.target_begin.value && pc < m.target_end.value) out.push_back(&m); return out;
}
FrontendRejected rejected(DirectFlowDiagnostic category, const FrontendProgram &p) {
  FrontendRejected r{};
  r.profile = p.profile;
  r.category = category;
  r.image_source_id = p.image.source_id;
  r.direct.category = category;
  return r;
}
void set_source(FrontendRejected &rejection, M68kProgramAddress address) {
  rejection.source_address = address;
  rejection.direct.block.entry = address;
}
void set_claims(FrontendRejected &rejection, std::vector<MappingClaim> claims) {
  rejection.mapping_claims = std::move(claims);
  rejection.direct.mapping_claims = *rejection.mapping_claims;
}
std::optional<std::size_t> block_index(const DirectFlowAnalysis &a, const M68kProgramAddress &entry) { for(std::size_t i=0;i<a.blocks.size();++i) if(same(a.blocks[i].provenance.id.entry,entry)) return i; return std::nullopt; }
// Shared JSR target policy: a direct static call target must be
// a 24-bit-clean, even, unambiguously mapped m68k program address before any
// callee decode is attempted. Both the eager decode-time check and the
// call/return discovery entry point must apply exactly this rule so a call
// target is never accepted by one path and rejected by the other.
std::optional<DirectFlowDiagnostic> validate_m68k_static_call_target(const FrontendProgram &program,
                                                                       std::uint32_t target) {
  if ((target & UINT32_C(0xFF000000)) != 0U) return DirectFlowDiagnostic::effective_address_not_24bit;
  if ((target & 1U) != 0U) return DirectFlowDiagnostic::odd_direct_target;
  const auto target_mappings = claims(program.mapping_claims, target);
  if (target_mappings.size() != 1U) return DirectFlowDiagnostic::unmapped_direct_target;
  return std::nullopt;
}
// Sole owner of "reject this call instruction because its static target
// failed validate_m68k_static_call_target": both the eager decode-time check
// and discover_m68k_static_call_return build the rejection through here so
// the two paths cannot drift on which fields are populated.
// `target` is threaded explicitly (see build_m68k_static_call's identical
// generalization above) so JMP/JSR's general `source_ea.absolute_address`
// target can reuse this exact rejection shape; every existing caller passes
// `call_decoded.extension` unchanged.
FrontendRejected build_static_call_target_rejection(const FrontendProgram &program,
                                                      const M68kDecodedInstruction &call_decoded,
                                                      DirectFlowDiagnostic category, std::uint32_t target) {
  auto r = rejected(category, program);
  set_source(r, call_decoded.provenance.source.address);
  r.image_offset = call_decoded.provenance.source.image_offset;
  r.provenance = call_decoded.provenance;
  r.direct.provenance = r.provenance.value();
  r.direct.has_provenance = true;
  r.instruction_length = call_decoded.provenance.length.value;
  r.direct.target = {TargetAddressSpace::m68k_program, target};
  r.direct.has_target = true;
  return r;
}

// Sole formula owner for a verified JSR call's identity
// (callee/continuation) and its call/return edges (SEG-007-T010). Both the
// immediate-pair discover_m68k_static_call_return route below and the
// general recursive discovery route (discover_m68k_general_startup) build
// every call/edge through these so the identity formula itself has exactly
// one owner regardless of how many callees a discovery path may nest.
// `callee_address` is threaded explicitly (rather than always reading
// `call_decoded.extension`) so the general whitelist's `jsr` kind -- whose
// statically-foldable target lives in `source_ea.absolute_address`, not
// `extension` -- can reuse this exact same identity formula; every existing
// caller passes `call_decoded.extension` unchanged, so this is a pure
// signature generalization with no behavior change for the selected JSR form.

// SEG-007-T023: the one general typed-EA legality/resolution call
// discover_m68k_general_startup's decode_instruction uses for every
// whitelisted kind's statically-foldable absolute/pc-relative operand,
// generalizing the two pre-existing narrow-form branches
// (the former exact MOVE forms' manual alignment+claims()+range check, and
// the former exact TST form's
// m68k_resolve_absolute_test_operand call) into one shared predicate. Per
// decision #3 ("static-vs-runtime EA split"), this is called ONLY for an
// operand whose EA mode is one of the three statically-foldable forms
// (absolute.w/absolute.l/d16(PC)); every other mode (Dn/An/immediate, and
// the four register-relative runtime-only modes) returns std::nullopt
// immediately -- static discovery can validate only EA-mode legality for
// those (already done at decode time), never a concrete address, since none
// exists yet.
// SEG-014-T003: the trimmed ROM-write/RAM/device-routing/ROM-read body,
// taking an already-canonical, already-alignment-verified `address` rather
// than an `M68kEffectiveAddress`. This is the sole remaining Genesis-owned
// content of the pre-T003 `m68k_resolve_static_general_operand`: the
// EA-mode-foldability and alignment checks it used to perform first are now
// owned by the CPU-side `discover_m68k_static_graph` boundary itself (pure
// MC68000 addressing-mode mechanics, per m68k_is_statically_foldable_
// control_ea/m68k_canonical_ea_address/m68k_startup_absolute_operand_
// alignment's own doc comments), so this trimmed body is exactly the Genesis
// environment adapter's own `classify_memory_access` implementation below.
// The legacy `m68k_resolve_static_general_operand` wrapper immediately below
// still performs those two checks itself before calling in, unchanged, for
// analyze_startup_profile's own two remaining direct call sites.
std::optional<DirectFlowDiagnostic> m68k_classify_general_memory_access(
    const FrontendProgram &program, std::uint32_t address, M68kMemoryAccessWidth width,
    M68kMemoryAccessDirection direction, const InstructionProvenance &provenance) {
  if (direction == M68kMemoryAccessDirection::write) {
    // Preserve the established region order: ROM is prohibited first, then
    // RAM is accepted, then an intersecting device request is routed through
    // the typed Genesis seam.  In particular, controller writes must not be
    // silently relabelled as generic unmapped accesses.
    const auto operand_mappings = claims(program.mapping_claims, address);
    if (!operand_mappings.empty()) return DirectFlowDiagnostic::rom_write_prohibited;
    if (m68k_startup_ram_range_in_range(address, static_cast<std::uint32_t>(width))) return std::nullopt;
    const auto routed = m68k_route_genesis_device_access(
        M68kMemoryAccessRequest{{TargetAddressSpace::m68k_program, address}, width, direction, provenance});
    if (const auto *failure = std::get_if<M68kControllerIoFailure>(&routed)) return failure->category;
    if (const auto *diagnostic = std::get_if<DirectFlowDiagnostic>(&routed)) return *diagnostic;
    // SEG-007-T113: a WORD/LONG store fully inside the VDP register window is
    // an explicitly routed shape -- the runtime device gate
    // (genesis_route_access -> genesis_vdp_access) owns it. The retained fact
    // (retain_fact above) carries only the routing decision; the runtime
    // stays the sole holder of VDP device state.
    if (std::holds_alternative<M68kVdpRoutedWrite>(routed)) return std::nullopt;
    // SEG-007-T115: a Z80 bus-arbitration / Z80 program-RAM / PSG store whose
    // shape the runtime device owner (genesis_route_access) already supports
    // is likewise an explicitly routed shape -- the runtime is the sole
    // holder of that device's state; the retained fact carries only the
    // routing decision.
    if (std::holds_alternative<M68kDeviceRoutedAccess>(routed)) return std::nullopt;
    // There is no other selected writable device shape in this batch.  A
    // future success here must receive an explicit lowering contract rather
    // than becoming an implicit store.
    return DirectFlowDiagnostic::unsupported_device_region_controller_io;
  }
  // A statically known ROM EA is in target program space, not an image
  // offset.  Translate it through its ordered mapping claim before handing a
  // bounded claim-local span to the shared ROM/RAM/device resolver.  The
  // resolver then reads only `local_address` from that verified span; source
  // provenance remains the CPU operation's original target provenance.
  const auto operand_mappings = claims(program.mapping_claims, address);
  if (!operand_mappings.empty()) {
    const auto *mapping = operand_mappings.front();
    const auto bytes_remaining = static_cast<std::uint64_t>(mapping->target_end.value) - address;
    if (bytes_remaining < static_cast<std::uint32_t>(width))
      return DirectFlowDiagnostic::unmapped_data_access;
    const auto image_begin = static_cast<std::size_t>(mapping->image_begin.value);
    const auto image_length = static_cast<std::size_t>(mapping->image_end.value - mapping->image_begin.value);
    const auto local_address = static_cast<std::uint32_t>(address - mapping->target_begin.value);
    const auto resolution = m68k_resolve_absolute_test_operand(
        std::span<const std::uint8_t>(program.image.bytes).subspan(image_begin, image_length), local_address,
        width, direction, provenance);
    if (const auto *diagnostic = std::get_if<DirectFlowDiagnostic>(&resolution)) return *diagnostic;
    if (const auto *controller_failure = std::get_if<M68kControllerIoFailure>(&resolution))
      return controller_failure->category;
    return std::nullopt;
  }
  const auto resolution =
      m68k_resolve_absolute_test_operand(program.image.bytes, address, width, direction, provenance);
  if (const auto *diagnostic = std::get_if<DirectFlowDiagnostic>(&resolution)) return *diagnostic;
  if (const auto *controller_failure = std::get_if<M68kControllerIoFailure>(&resolution))
    return controller_failure->category;
  return std::nullopt;
}

// SEG-007-T023: the one general typed-EA legality/resolution call
// analyze_startup_profile's own decode_selected uses for the fixed
// genesis_rom_startup graph's two MOVE operand positions, generalizing the
// two pre-existing narrow-form branches (the former exact MOVE forms' manual
// alignment+claims()+range check, and the former exact TST form's
// m68k_resolve_absolute_test_operand call) into one shared predicate. Per
// decision #3 ("static-vs-runtime EA split"), this is called ONLY for an
// operand whose EA mode is one of the three statically-foldable forms
// (absolute.w/absolute.l/d16(PC)); every other mode (Dn/An/immediate, and
// the four register-relative runtime-only modes) returns std::nullopt
// immediately -- static discovery can validate only EA-mode legality for
// those (already done at decode time), never a concrete address, since none
// exists yet.
std::optional<DirectFlowDiagnostic> m68k_resolve_static_general_operand(
    const FrontendProgram &program, const M68kEffectiveAddress &ea, M68kMemoryAccessWidth width,
    M68kMemoryAccessDirection direction, const InstructionProvenance &provenance) {
  if (ea.mode != M68kEaMode::absolute_word && ea.mode != M68kEaMode::absolute_long &&
       ea.mode != M68kEaMode::pc_disp16)
    return std::nullopt;
  // Preserve ea.absolute_address itself as decoded/lifted provenance.  This
  // is the sole static EA-to-Genesis-bus handoff before alignment and region
  // resolution, so only absolute.w receives 24-bit canonicalization.
  const auto address = m68k_canonical_ea_address(ea);
  if (const auto misaligned = m68k_startup_absolute_operand_alignment(address, width)) return *misaligned;
  return m68k_classify_general_memory_access(program, address, width, direction, provenance);
}

// SEG-014-T003: the Genesis general-startup scenario's own
// SEG-007-T164 / ADR-0023: a minimal, narrow JSON reader scoped exactly to
// the external hints interchange file's own schema (an object, or an array
// of objects, with string/number/bool/null leaf values and no `\uXXXX`
// escapes). This is deliberately not a general-purpose JSON library and must
// not be reused for any other purpose. Any structural violation anywhere in
// the input causes `JsonParser::parse` to return `std::nullopt`, which
// `parse_genesis_external_hints` below treats as "zero hints" -- a hints
// file is never a hard build failure, per ADR-0023's fail-closed contract.
struct JsonValue {
  enum class Kind { null_value, boolean, number, string, array, object } kind{Kind::null_value};
  bool boolean_value{};
  std::string number_lexeme;
  std::string string_value;
  std::vector<JsonValue> array_value;
  std::vector<std::pair<std::string, JsonValue>> object_value;
  [[nodiscard]] const JsonValue *field(std::string_view name) const {
    for (const auto &entry : object_value)
      if (entry.first == name) return &entry.second;
    return nullptr;
  }
};

class JsonParser {
 public:
  explicit JsonParser(std::string_view text) : text_(text) {}
  [[nodiscard]] std::optional<JsonValue> parse() {
    skip_ws();
    auto value = parse_value(0U);
    if (!value) return std::nullopt;
    skip_ws();
    if (pos_ != text_.size()) return std::nullopt;  // trailing garbage after the top-level value
    return value;
  }

 private:
  // External hints are untrusted and this narrow parser is recursive. Keep
  // the accepted interchange shape comfortably above the descriptor schema's
  // needs while making host-stack use independent of input nesting.
  static constexpr std::size_t max_nesting_depth = 64U;

  void skip_ws() {
    while (pos_ < text_.size() &&
           (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r'))
      ++pos_;
  }
  [[nodiscard]] std::optional<JsonValue> parse_value(std::size_t depth) {
    if (pos_ >= text_.size()) return std::nullopt;
    const char c = text_[pos_];
    if (c == '{') return parse_object(depth);
    if (c == '[') return parse_array(depth);
    if (c == '"') return parse_string_value();
    if (c == 't' || c == 'f') return parse_bool();
    if (c == 'n') return parse_null();
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
    return std::nullopt;
  }
  [[nodiscard]] std::optional<JsonValue> parse_object(std::size_t depth) {
    if (depth >= max_nesting_depth) return std::nullopt;
    JsonValue out;
    out.kind = JsonValue::Kind::object;
    ++pos_;
    skip_ws();
    if (pos_ < text_.size() && text_[pos_] == '}') { ++pos_; return out; }
    for (;;) {
      skip_ws();
      auto key = parse_string();
      if (!key) return std::nullopt;
      skip_ws();
      if (pos_ >= text_.size() || text_[pos_] != ':') return std::nullopt;
      ++pos_;
      skip_ws();
      auto value = parse_value(depth + 1U);
      if (!value) return std::nullopt;
      // RFC 8259 only says object names SHOULD be unique, but this trusted-
      // hint reader cannot safely assign "first" or "last" authority to an
      // ambiguous semantic/provenance field. Reject duplicates at every
      // nesting level instead (including inside `provenance`).
      if (std::any_of(out.object_value.begin(), out.object_value.end(),
                      [&](const auto &entry) { return entry.first == *key; }))
        return std::nullopt;
      out.object_value.emplace_back(std::move(*key), std::move(*value));
      skip_ws();
      if (pos_ >= text_.size()) return std::nullopt;
      if (text_[pos_] == ',') { ++pos_; continue; }
      if (text_[pos_] == '}') { ++pos_; break; }
      return std::nullopt;
    }
    return out;
  }
  [[nodiscard]] std::optional<JsonValue> parse_array(std::size_t depth) {
    if (depth >= max_nesting_depth) return std::nullopt;
    JsonValue out;
    out.kind = JsonValue::Kind::array;
    ++pos_;
    skip_ws();
    if (pos_ < text_.size() && text_[pos_] == ']') { ++pos_; return out; }
    for (;;) {
      skip_ws();
      auto value = parse_value(depth + 1U);
      if (!value) return std::nullopt;
      out.array_value.push_back(std::move(*value));
      skip_ws();
      if (pos_ >= text_.size()) return std::nullopt;
      if (text_[pos_] == ',') { ++pos_; continue; }
      if (text_[pos_] == ']') { ++pos_; break; }
      return std::nullopt;
    }
    return out;
  }
  [[nodiscard]] std::optional<std::string> parse_string() {
    if (pos_ >= text_.size() || text_[pos_] != '"') return std::nullopt;
    ++pos_;
    std::string out;
    while (pos_ < text_.size() && text_[pos_] != '"') {
      const char c = text_[pos_];
      if (c == '\\') {
        ++pos_;
        if (pos_ >= text_.size()) return std::nullopt;
        switch (text_[pos_]) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        default: return std::nullopt;  // \uXXXX and other escapes are unsupported by this narrow reader
        }
        ++pos_;
      } else {
        if (static_cast<unsigned char>(c) < 0x20U) return std::nullopt;
        out.push_back(c);
        ++pos_;
      }
    }
    if (pos_ >= text_.size()) return std::nullopt;  // unterminated string
    ++pos_;  // closing quote
    return out;
  }
  [[nodiscard]] std::optional<JsonValue> parse_string_value() {
    auto value = parse_string();
    if (!value) return std::nullopt;
    JsonValue out;
    out.kind = JsonValue::Kind::string;
    out.string_value = std::move(*value);
    return out;
  }
  [[nodiscard]] std::optional<JsonValue> parse_bool() {
    if (text_.substr(pos_, 4) == "true") {
      pos_ += 4;
      JsonValue out; out.kind = JsonValue::Kind::boolean; out.boolean_value = true;
      return out;
    }
    if (text_.substr(pos_, 5) == "false") {
      pos_ += 5;
      JsonValue out; out.kind = JsonValue::Kind::boolean; out.boolean_value = false;
      return out;
    }
    return std::nullopt;
  }
  [[nodiscard]] std::optional<JsonValue> parse_null() {
    if (text_.substr(pos_, 4) == "null") {
      pos_ += 4;
      JsonValue out; out.kind = JsonValue::Kind::null_value;
      return out;
    }
    return std::nullopt;
  }
  [[nodiscard]] std::optional<JsonValue> parse_number() {
    const auto start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
    // JSON number grammar is deliberately recognized before conversion:
    // integer = zero / (digit1-9 *DIGIT), and fraction/exponent components
    // each require at least one digit. `from_chars` alone is prefix-oriented
    // and must not decide where a JSON token ends.
    if (pos_ >= text_.size()) return std::nullopt;
    if (text_[pos_] == '0') {
      ++pos_;
      if (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') return std::nullopt;
    } else if (text_[pos_] >= '1' && text_[pos_] <= '9') {
      do { ++pos_; } while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9');
    } else {
      return std::nullopt;
    }
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      const auto fraction_start = pos_;
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
      if (pos_ == fraction_start) return std::nullopt;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
      const auto exponent_start = pos_;
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
      if (pos_ == exponent_start) return std::nullopt;
    }
    JsonValue out;
    out.kind = JsonValue::Kind::number;
    out.number_lexeme = std::string(text_.substr(start, pos_ - start));
    return out;
  }
  std::string_view text_;
  std::size_t pos_{};
};

[[nodiscard]] std::optional<std::string> json_string(const JsonValue *value) {
  if (value == nullptr || value->kind != JsonValue::Kind::string) return std::nullopt;
  return value->string_value;
}

// Accepts either a JSON number or a JSON string ("123" or a "0x..."/"0X..."
// hex literal) for one interchange-format scalar field, matching how a
// human-authored or tool-generated hints file may reasonably spell an
// address/count. `std::nullopt` on any missing field, wrong JSON type,
// malformed literal, negative value, non-integral numeric value, or
// out-of-range value. SEG-007-T164 hardening: a JSON number that is not
// mathematically integral (e.g. `1.5`) is rejected outright rather than
// silently truncated -- these fields are byte counts/addresses/widths, never
// fractional quantities, so a non-integral literal is evidence of a
// malformed or misauthored record, not a value this reader may guess at.
[[nodiscard]] std::optional<std::uint32_t> json_uint32(const JsonValue *value) {
  if (value == nullptr) return std::nullopt;
  if (value->kind == JsonValue::Kind::number) {
    // Convert the retained JSON token exactly. A binary floating-point round
    // trip could otherwise turn a tiny nonzero fraction into an integer and
    // grant it authority as an address/count.
    const std::string_view token = value->number_lexeme;
    std::size_t begin = 0U;
    const bool negative = !token.empty() && token.front() == '-';
    if (negative) begin = 1U;
    const auto exponent_mark = token.find_first_of("eE", begin);
    const auto mantissa_end = exponent_mark == std::string_view::npos ? token.size() : exponent_mark;
    const auto decimal_mark = token.find('.', begin);
    const std::size_t fraction_digits =
        decimal_mark != std::string_view::npos && decimal_mark < mantissa_end
            ? mantissa_end - decimal_mark - 1U
            : 0U;

    std::int64_t exponent = 0;
    if (exponent_mark != std::string_view::npos) {
      std::size_t exponent_pos = exponent_mark + 1U;
      bool exponent_negative = false;
      if (token[exponent_pos] == '+' || token[exponent_pos] == '-') {
        exponent_negative = token[exponent_pos] == '-';
        ++exponent_pos;
      }
      std::uint64_t magnitude = 0U;
      for (; exponent_pos < token.size(); ++exponent_pos) {
        const auto digit = static_cast<std::uint64_t>(token[exponent_pos] - '0');
        if (magnitude > (static_cast<std::uint64_t>(INT64_MAX) - digit) / 10U)
          return std::nullopt;
        magnitude = magnitude * 10U + digit;
      }
      exponent = exponent_negative ? -static_cast<std::int64_t>(magnitude)
                                   : static_cast<std::int64_t>(magnitude);
    }

    bool all_zero = true;
    std::size_t digit_count = 0U;
    std::size_t trailing_zeroes = 0U;
    for (std::size_t i = begin; i < mantissa_end; ++i) {
      if (token[i] == '.') continue;
      ++digit_count;
      if (token[i] == '0') {
        ++trailing_zeroes;
      } else {
        all_zero = false;
        trailing_zeroes = 0U;
      }
    }
    if (all_zero) return 0U;  // Includes mathematically exact negative zero.
    if (negative || fraction_digits > static_cast<std::size_t>(INT64_MAX) ||
        exponent < INT64_MIN + static_cast<std::int64_t>(fraction_digits))
      return std::nullopt;
    const std::int64_t decimal_shift = exponent - static_cast<std::int64_t>(fraction_digits);
    std::size_t discarded_digits = 0U;
    if (decimal_shift < 0) {
      if (decimal_shift == INT64_MIN) return std::nullopt;
      const auto required_zeroes = static_cast<std::uint64_t>(-decimal_shift);
      if (required_zeroes > trailing_zeroes) return std::nullopt;
      discarded_digits = static_cast<std::size_t>(required_zeroes);
    } else if (decimal_shift > 10) {
      return std::nullopt;  // Every nonzero coefficient is then above uint32.
    }

    const std::size_t kept_digits = digit_count - discarded_digits;
    std::uint64_t result = 0U;
    std::size_t seen_digits = 0U;
    for (std::size_t i = begin; i < mantissa_end && seen_digits < kept_digits; ++i) {
      if (token[i] == '.') continue;
      const auto digit = static_cast<std::uint64_t>(token[i] - '0');
      if (result > (UINT32_MAX - digit) / 10U) return std::nullopt;
      result = result * 10U + digit;
      ++seen_digits;
    }
    for (std::int64_t i = 0; i < decimal_shift; ++i) {
      if (result > UINT32_MAX / 10U) return std::nullopt;
      result *= 10U;
    }
    return static_cast<std::uint32_t>(result);
  }
  if (value->kind == JsonValue::Kind::string) {
    std::string_view text = value->string_value;
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
      text.remove_prefix(2);
      base = 16;
    }
    if (text.empty()) return std::nullopt;
    std::uint64_t out{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out, base);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || out > 0xFFFFFFFFULL)
      return std::nullopt;
    return static_cast<std::uint32_t>(out);
  }
  return std::nullopt;
}

// Validates and converts one candidate JSON object into a
// `GenesisLogicalTableDescriptorHint`, or `std::nullopt` on any of ADR-0023's
// fail-closed conditions: not an object; `kind` missing or not exactly
// `"logical_table_descriptor"` (a record of a different/unrecognized kind is
// silently skipped, not an error -- the interchange format is deliberately
// forward-extensible); `rom_sha256` missing or not exactly equal to
// `expected_rom_sha256` (the ROM-identity precondition); any required scalar
// field (`base_address`, `entry_width_bytes`, `stride_bytes`, `entry_count`)
// missing, malformed, or zero; or a missing/malformed `provenance` object.
// SEG-007-T164 hardening: the provenance contract (ADR-0023 "Provenance in
// output") is REQUIRED, not decorative -- a record missing the `provenance`
// object entirely, or missing any of its four required typed fields
// (`tool`/`tool_version`/`timestamp` as strings, `human_reviewed` as a JSON
// boolean), is dropped rather than accepted with empty/default provenance.
[[nodiscard]] std::optional<GenesisLogicalTableDescriptorHint> parse_one_external_hint(
    const JsonValue &object, std::string_view expected_rom_sha256) {
  if (object.kind != JsonValue::Kind::object) return std::nullopt;
  const auto kind = json_string(object.field("kind"));
  if (!kind || *kind != "logical_table_descriptor") return std::nullopt;
  const auto rom_sha256 = json_string(object.field("rom_sha256"));
  if (!rom_sha256 || *rom_sha256 != expected_rom_sha256) return std::nullopt;
  const auto base_address = json_uint32(object.field("base_address"));
  const auto entry_width_bytes = json_uint32(object.field("entry_width_bytes"));
  const auto stride_bytes = json_uint32(object.field("stride_bytes"));
  const auto entry_count = json_uint32(object.field("entry_count"));
  if (!base_address || !entry_width_bytes || !stride_bytes || !entry_count) return std::nullopt;
  if (*stride_bytes == 0U || *entry_count == 0U) return std::nullopt;
  const auto *provenance = object.field("provenance");
  if (provenance == nullptr || provenance->kind != JsonValue::Kind::object) return std::nullopt;
  const auto tool = json_string(provenance->field("tool"));
  const auto tool_version = json_string(provenance->field("tool_version"));
  const auto timestamp = json_string(provenance->field("timestamp"));
  const auto *reviewed = provenance->field("human_reviewed");
  if (!tool || !tool_version || !timestamp || reviewed == nullptr || reviewed->kind != JsonValue::Kind::boolean)
    return std::nullopt;
  GenesisLogicalTableDescriptorHint out{};
  out.base_address = *base_address;
  out.entry_width_bytes = *entry_width_bytes;
  out.stride_bytes = *stride_bytes;
  out.entry_count = *entry_count;
  out.provenance_tool = *tool;
  out.provenance_tool_version = *tool_version;
  out.provenance_timestamp = *timestamp;
  out.provenance_human_reviewed = reviewed->boolean_value;
  return out;
}

// SEG-007-T174 / ADR-0024: validates and converts one candidate JSON object
// into a `GenesisCodeEntryCandidateHint`, or `std::nullopt` when: not an
// object; `kind` missing or not exactly `"code_entry_candidate"` (a record of
// a different/unrecognized kind is silently skipped -- the interchange
// format stays forward-extensible, exactly like
// `parse_one_external_hint`); `rom_sha256` missing or not exactly equal to
// `expected_rom_sha256`; `address` missing/malformed; or a missing/malformed
// `provenance` object (the same required-provenance contract ADR-0023
// established, applied here verbatim).
[[nodiscard]] std::optional<GenesisCodeEntryCandidateHint> parse_one_code_entry_candidate(
    const JsonValue &object, std::string_view expected_rom_sha256) {
  if (object.kind != JsonValue::Kind::object) return std::nullopt;
  const auto kind = json_string(object.field("kind"));
  if (!kind || *kind != "code_entry_candidate") return std::nullopt;
  const auto rom_sha256 = json_string(object.field("rom_sha256"));
  if (!rom_sha256 || *rom_sha256 != expected_rom_sha256) return std::nullopt;
  const auto address = json_uint32(object.field("address"));
  if (!address) return std::nullopt;
  const auto *provenance = object.field("provenance");
  if (provenance == nullptr || provenance->kind != JsonValue::Kind::object) return std::nullopt;
  const auto tool = json_string(provenance->field("tool"));
  const auto tool_version = json_string(provenance->field("tool_version"));
  const auto timestamp = json_string(provenance->field("timestamp"));
  const auto *reviewed = provenance->field("human_reviewed");
  if (!tool || !tool_version || !timestamp || reviewed == nullptr || reviewed->kind != JsonValue::Kind::boolean)
    return std::nullopt;
  GenesisCodeEntryCandidateHint out{};
  out.address = *address;
  out.provenance_tool = *tool;
  out.provenance_tool_version = *tool_version;
  out.provenance_timestamp = *timestamp;
  out.provenance_human_reviewed = reviewed->boolean_value;
  return out;
}

[[nodiscard]] std::optional<GenesisCodePointerTableDescriptorHint>
parse_one_code_pointer_table_descriptor(const JsonValue &object,
                                        std::string_view expected_rom_sha256) {
  if (object.kind != JsonValue::Kind::object) return std::nullopt;
  const auto kind = json_string(object.field("kind"));
  if (!kind || *kind != "code_pointer_table_descriptor") return std::nullopt;
  const auto rom_sha256 = json_string(object.field("rom_sha256"));
  const auto source = json_string(object.field("source"));
  const auto pointer_type = json_string(object.field("pointer_type"));
  const auto base_address = json_uint32(object.field("base_address"));
  const auto entry_width_bytes = json_uint32(object.field("entry_width_bytes"));
  const auto stride_bytes = json_uint32(object.field("stride_bytes"));
  const auto entry_count = json_uint32(object.field("entry_count"));
  if (!rom_sha256 || *rom_sha256 != expected_rom_sha256 || !source ||
      *source != "immutable_cartridge" || !pointer_type ||
      *pointer_type != "absolute_code_address" || !base_address ||
      !entry_width_bytes || *entry_width_bytes != 4U || !stride_bytes ||
      *stride_bytes != 4U || !entry_count || *entry_count == 0U ||
      *entry_count > genesis_code_pointer_table_max_entries)
    return std::nullopt;
  const std::uint64_t span = static_cast<std::uint64_t>(*entry_count) * 4U;
  if (static_cast<std::uint64_t>(*base_address) + span > UINT64_C(0x1'0000'0000))
    return std::nullopt;
  const auto *provenance = object.field("provenance");
  if (provenance == nullptr || provenance->kind != JsonValue::Kind::object) return std::nullopt;
  const auto tool = json_string(provenance->field("tool"));
  const auto tool_version = json_string(provenance->field("tool_version"));
  const auto timestamp = json_string(provenance->field("timestamp"));
  const auto *reviewed = provenance->field("human_reviewed");
  if (!tool || tool->empty() || !tool_version || tool_version->empty() || !timestamp || timestamp->empty() ||
      reviewed == nullptr || reviewed->kind != JsonValue::Kind::boolean || !reviewed->boolean_value)
    return std::nullopt;
  return GenesisCodePointerTableDescriptorHint{*base_address, *entry_count, *tool,
                                                *tool_version, *timestamp, true};
}

// SEG-007-T204 / ADR-0033: parses one raw `address_table_candidate` record.
// Unlike `parse_one_code_pointer_table_descriptor`, `human_reviewed` is NOT
// required to be `true` here -- this record is never itself trusted, so its
// provenance carries no reviewed-vs-unreviewed authority distinction; the
// field is still required to be present and well-formed (schema
// completeness), but its actual boolean value is recorded, not gated on.
[[nodiscard]] std::optional<GenesisAddressTableCandidateHint>
parse_one_address_table_candidate(const JsonValue &object, std::string_view expected_rom_sha256) {
  if (object.kind != JsonValue::Kind::object) return std::nullopt;
  const auto kind = json_string(object.field("kind"));
  if (!kind || *kind != "address_table_candidate") return std::nullopt;
  const auto rom_sha256 = json_string(object.field("rom_sha256"));
  const auto base_address = json_uint32(object.field("base_address"));
  const auto entry_width_bytes = json_uint32(object.field("entry_width_bytes"));
  const auto stride_bytes = json_uint32(object.field("stride_bytes"));
  const auto entry_count = json_uint32(object.field("entry_count"));
  if (!rom_sha256 || *rom_sha256 != expected_rom_sha256 || !base_address ||
      !entry_width_bytes || *entry_width_bytes != 4U || !stride_bytes ||
      *stride_bytes != 4U || !entry_count || *entry_count == 0U ||
      *entry_count > genesis_code_pointer_table_max_entries)
    return std::nullopt;
  const std::uint64_t span = static_cast<std::uint64_t>(*entry_count) * 4U;
  if (static_cast<std::uint64_t>(*base_address) + span > UINT64_C(0x1'0000'0000))
    return std::nullopt;
  const auto *provenance = object.field("provenance");
  if (provenance == nullptr || provenance->kind != JsonValue::Kind::object) return std::nullopt;
  const auto tool = json_string(provenance->field("tool"));
  const auto tool_version = json_string(provenance->field("tool_version"));
  const auto timestamp = json_string(provenance->field("timestamp"));
  const auto *reviewed = provenance->field("human_reviewed");
  if (!tool || tool->empty() || !tool_version || tool_version->empty() || !timestamp || timestamp->empty() ||
      reviewed == nullptr || reviewed->kind != JsonValue::Kind::boolean)
    return std::nullopt;
  return GenesisAddressTableCandidateHint{*base_address, *entry_width_bytes, *stride_bytes, *entry_count,
                                           *tool, *tool_version, *timestamp, reviewed->boolean_value};
}

// M68kStaticDiscoveryEnvironment implementation, consumed only by
// discover_m68k_general_startup below. Every method answers exactly the fact
// question discover_m68k_static_graph's own boundary interface asks; it
// makes no walk/recurse/successor-set/frame-stack decision of its own -- see
// that interface's own doc comments for the full division of responsibility.
class M68kGeneralStartupEnvironment final : public M68kStaticDiscoveryEnvironment {
 public:
  explicit M68kGeneralStartupEnvironment(const FrontendProgram &program) : program_(program) {}

  M68kInstructionSourceResult instruction_source(M68kProgramAddress pc) override {
    const auto found = claims(program_.mapping_claims, pc.value);
    if (found.size() != 1U) {
      M68kInstructionSourceIssue issue{};
      issue.kind = found.empty() ? M68kInstructionSourceIssueKind::unmapped
                                  : M68kInstructionSourceIssueKind::conflicting_mapping;
      issue.address = pc;
      for (const auto *claim : found) issue.matched_claims.push_back(*claim);
      return issue;
    }
    const auto &claim = *found.front();
    const auto local = static_cast<std::size_t>(pc.value - claim.target_begin.value);
    const auto claim_bytes = std::span<const std::uint8_t>(program_.image.bytes)
        .subspan(static_cast<std::size_t>(claim.image_begin.value),
                 static_cast<std::size_t>(claim.image_end.value - claim.image_begin.value));
    const DecodeSource source{CpuVariant::mc68000, pc, {local}};
    return M68kInstructionSource{claim_bytes, source, {claim.image_begin.value + local}};
  }

  std::optional<M68kMappingIssue> admit_target(M68kProgramAddress target, M68kDiscoveryTargetRole role) override {
    if (role == M68kDiscoveryTargetRole::direct_branch) {
      // Mirrors the pre-T003 monolith's own validate_branch_target mapping
      // section: odd and mid-instruction checks stay CPU-side (already
      // performed by the caller before this is ever reached).
      const auto found = claims(program_.mapping_claims, target.value);
      if (found.size() > 1U) {
        M68kMappingIssue issue{DirectFlowDiagnostic::conflicting_address_mapping, {}};
        for (const auto *claim : found) issue.matched_claims.push_back(*claim);
        return issue;
      }
      if (found.empty()) return M68kMappingIssue{DirectFlowDiagnostic::unmapped_direct_target, {}};
      return std::nullopt;
    }
    // direct_call: validate_m68k_static_call_target owns its own odd/24-bit/
    // mapped admission rule and its own single collapsed
    // unmapped_direct_target category (never conflicting_address_mapping,
    // never a claims list) -- preserved verbatim, unchanged.
    if (const auto category = validate_m68k_static_call_target(program_, target.value))
      return M68kMappingIssue{*category, {}};
    return std::nullopt;
  }

  std::optional<DirectFlowDiagnostic> classify_memory_access(const M68kCpuMemoryAccessRequest &request) override {
    return m68k_classify_general_memory_access(program_, request.address.value, request.width, request.direction,
                                                request.provenance);
  }

  bool is_completion_rts(const InstructionProvenance &rts_provenance) override {
    return program_.synthetic_completion.has_value() &&
           same(program_.synthetic_completion->terminal_rts_address, rts_provenance.source.address);
  }

  // SEG-007-T161 / ADR-0022: one bounded generation-time read of immutable
  // in-cartridge bytes for the ADR-0009 offset-table finite-value producer.
  // Reuses the `raw_cartridge_rom` immutability signal and
  // `structurally_valid_mapping_claim`'s affine arithmetic -- the same trust
  // every instruction fetch already relies on. A successful read proves that
  // exactly one MappingClaim overlaps ANY byte of the requested target
  // interval `[base, base+length)` and that this single claim is structurally
  // valid and completely covers the interval. Fails closed (`std::nullopt`)
  // on absent, partial, conflicting, interior-overlapping, overflowed, or
  // otherwise ambiguous coverage.
  std::optional<M68kImmutableCartridgeBytes> read_immutable_cartridge_bytes(
      M68kProgramAddress base, std::uint32_t length) override {
    if (length == 0U) return std::nullopt;
    // Prove ONE unambiguous whole-descriptor range read (ADR-0022). The
    // requested target interval is half-open `[base, end)`. Repository mapping
    // contracts permit overlapping target MappingClaims (see `claims()`), so a
    // second structurally valid claim can overlap only an INTERIOR subrange
    // while neither endpoint is ambiguous. Endpoint-only sampling would wrongly
    // accept that; instead iterate every claim once and collect every claim
    // whose target interval overlaps `[base, end)`. Exactly one such claim may
    // exist; it must be structurally valid AND fully contain `[base, end)`.
    // Absent, partial, conflicting, or overflowed coverage fails closed.
    const auto end = static_cast<std::uint64_t>(base.value) + length;
    if (end > UINT64_C(0x1'0000'0000)) return std::nullopt;
    const MappingClaim *covering = nullptr;
    for (const auto &candidate : program_.mapping_claims) {
      if (candidate.target_begin.space != TargetAddressSpace::m68k_program ||
          candidate.target_end.space != TargetAddressSpace::m68k_program)
        continue;
      // Half-open overlap test against `[base, end)`.
      if (!(base.value < candidate.target_end.value && candidate.target_begin.value < end)) continue;
      if (covering != nullptr) return std::nullopt;  // >1 overlapping claim: ambiguous.
      covering = &candidate;
    }
    if (covering == nullptr) return std::nullopt;
    const auto &claim = *covering;
    if (!structurally_valid_mapping_claim(claim)) return std::nullopt;
    // The single overlapping claim must completely cover `[base, end)`.
    if (base.value < claim.target_begin.value || end > claim.target_end.value) return std::nullopt;
    const auto local = static_cast<std::uint64_t>(base.value) - claim.target_begin.value;
    const auto image_off = static_cast<std::uint64_t>(claim.image_begin.value) + local;
    if (image_off + length > claim.image_end.value) return std::nullopt;
    if (image_off + length > program_.image.bytes.size()) return std::nullopt;
    M68kImmutableCartridgeBytes out{};
    out.bytes.assign(program_.image.bytes.begin() + static_cast<std::ptrdiff_t>(image_off),
                     program_.image.bytes.begin() + static_cast<std::ptrdiff_t>(image_off + length));
    out.claim = claim;
    return out;
  }

  // SEG-007-T164 / ADR-0023: linear scan of the (small, bounded) opted-in,
  // already ROM-hash-verified hint list for an exact `base_address` and
  // `entry_width_bytes` match. `program_.external_logical_table_descriptor_hints`
  // is empty for every scenario that never opts into `--external-hints`, so
  // this always returns `std::nullopt` in that (default) case.
  std::optional<M68kLogicalTableDescriptorHint> logical_table_descriptor_hint(
      M68kProgramAddress base_address, std::uint32_t entry_width_bytes) override {
    for (const auto &hint : program_.external_logical_table_descriptor_hints) {
      if (hint.base_address == base_address.value && hint.entry_width_bytes == entry_width_bytes)
        return M68kLogicalTableDescriptorHint{hint.stride_bytes, hint.entry_count};
    }
    return std::nullopt;
  }

 private:
  const FrontendProgram &program_;
};

// SEG-014-T003: reconstructs the exact same FrontendRejected shape the
// pre-T003 monolith's own inline call sites built directly, from the
// CPU-owned M68kDiscoveryIssue discover_m68k_static_graph now returns. See
// M68kDiscoveryIssue's own doc comments for exactly which reconstruction
// each of its boolean flags requests. The small duplicated mapping-claim/
// image-byte lookup below is a deliberate, explicitly-labeled adapter-side
// re-derivation of a fact discovery already established once (the address
// was already uniquely claimed by the time any of these issues could ever
// be produced) purely to reconstruct this scenario's own established report
// shape; it never reintroduces a control-flow decision.
FrontendRejected translate_m68k_discovery_issue(const FrontendProgram &program, const M68kDiscoveryIssue &issue) {
  // The one nested override the pre-T003 monolith's own decode_instruction
  // could produce: a decode issue whose own verified provenance length
  // genuinely exceeds even its containing claim's own remaining bytes
  // replaces the ENTIRE rejection with a bare truncated_instruction,
  // regardless of which decode-issue category was originally intended.
  if (issue.reconstruct_instruction_read_access && issue.provenance) {
    const auto &p = *issue.provenance;
    if (p.length.value > p.bytes.size()) {
      const auto found = claims(program.mapping_claims, issue.address.value);
      if (found.size() == 1U) {
        const auto &claim = *found.front();
        const auto local = static_cast<std::size_t>(issue.address.value - claim.target_begin.value);
        const auto claim_bytes = std::span<const std::uint8_t>(program.image.bytes)
            .subspan(static_cast<std::size_t>(claim.image_begin.value),
                     static_cast<std::size_t>(claim.image_end.value - claim.image_begin.value));
        if (p.length.value > claim_bytes.size() - local) {
          auto truncated = rejected(DirectFlowDiagnostic::truncated_instruction, program);
          set_source(truncated, issue.address);
          set_claims(truncated, {claim});
          return truncated;
        }
      }
    }
  }

  auto r = rejected(issue.category, program);
  set_source(r, issue.address);
  if (issue.set_pc_based_image_offset) {
    const auto found = claims(program.mapping_claims, issue.address.value);
    if (found.size() == 1U) {
      const auto &claim = *found.front();
      const auto local = issue.address.value - claim.target_begin.value;
      r.image_offset = MoveqImageOffset{claim.image_begin.value + local};
    }
  } else if (issue.provenance) {
    r.image_offset = issue.provenance->source.image_offset;
  }
  if (issue.provenance) {
    r.provenance = issue.provenance;
    r.direct.provenance = *issue.provenance;
    r.direct.has_provenance = true;
  }
  r.instruction_length = issue.instruction_length;
  r.available_bytes = issue.available_bytes;
  r.requested_length = issue.requested_length;
  if (issue.target) {
    r.direct.target = *issue.target;
    r.direct.has_target = true;
  }
  if (issue.unresolved_reason) r.direct.unresolved_reason = *issue.unresolved_reason;
  if (issue.mapping_claims) {
    set_claims(r, *issue.mapping_claims);
  } else if (issue.reconstruct_single_mapping_claim) {
    const auto found = claims(program.mapping_claims, issue.address.value);
    if (found.size() == 1U) set_claims(r, {*found.front()});
  }
  if (issue.reconstruct_instruction_read_access && issue.provenance) {
    const auto &p = *issue.provenance;
    std::vector<std::uint8_t> fetch_bytes(p.bytes.begin(), p.bytes.end());
    const auto found = claims(program.mapping_claims, issue.address.value);
    if (found.size() == 1U) {
      const auto &claim = *found.front();
      const auto local = static_cast<std::size_t>(issue.address.value - claim.target_begin.value);
      const auto claim_bytes = std::span<const std::uint8_t>(program.image.bytes)
          .subspan(static_cast<std::size_t>(claim.image_begin.value),
                   static_cast<std::size_t>(claim.image_end.value - claim.image_begin.value));
      if (p.length.value > p.bytes.size()) {
        fetch_bytes.assign(claim_bytes.begin() + static_cast<std::ptrdiff_t>(local),
                            claim_bytes.begin() + static_cast<std::ptrdiff_t>(local + p.length.value));
      }
    }
    r.accesses.push_back({0U, StartupBusKind::instruction_read, p.source.address, std::move(fetch_bytes),
                           "raw_cartridge_rom", p});
  }
  return r;
}

// ADR-0021 §1/§2a: a StaticProgramRoot is either a synchronous entry root
// (an ADR-0013 seed, `ordinal` = its index into `S`), an asynchronous hardware
// root (IRQ6), or a synchronous exception root (vector 5). This is purely a merge/
// fact-provenance and cross-root supersession bookkeeping identity (ADR-0021
// §2a); it never becomes a member of ADR-0013's seed set `S` and is
// deliberately kept file-local rather than a new public header type.
enum class StaticProgramRootKind { synchronous_entry, asynchronous_hardware, synchronous_exception };
struct StaticProgramRootId {
  StaticProgramRootKind kind;
  std::size_t ordinal;
  bool operator==(const StaticProgramRootId &other) const {
    return kind == other.kind && ordinal == other.ordinal;
  }
  bool operator!=(const StaticProgramRootId &other) const { return !(*this == other); }
};

// SEG-007-T064: shared classification, reused for the primary failure and
// every best-effort secondary -- ADR-0021 §4 additionally reuses this exact
// machinery, unmodified, to detect the IRQ6 root's own fatal boundary-
// probe-failure discriminator directly from its raw discovery result, before
// that result is folded into the shared aggregation.
// ADR 0013 Decision §6/§3: discovery_budget_exhausted classifies as
// discovery_prefix_boundary if and only if the Decision §3 discriminator
// holds (empty direct.unresolved_reason and direct.has_target == false);
// every other budget-exhausted diagnostic (block-entry cap, call-admission
// cap) keeps a non-empty unresolved_reason or a set target and falls
// through to std::nullopt, exactly like today's whole-program rejection.
std::optional<GenesisFrontierClass> classify_frontier(const FrontendRejected &diagnostic, bool has_frontier_access) {
  switch (diagnostic.category) {
  case DirectFlowDiagnostic::valid_but_unsupported_instruction:
  case DirectFlowDiagnostic::unsupported_instruction_form:
    return GenesisFrontierClass::unsupported_cpu_form;
  case DirectFlowDiagnostic::unsupported_device_region_controller_io:
    return has_frontier_access ? std::optional{GenesisFrontierClass::unsupported_device_access} : std::nullopt;
  case DirectFlowDiagnostic::unmapped_data_access:
    return has_frontier_access ? std::optional{GenesisFrontierClass::unsupported_memory_region} : std::nullopt;
  case DirectFlowDiagnostic::reached_unresolved_direct_edge:
    return GenesisFrontierClass::unresolved_indirect_target;
  case DirectFlowDiagnostic::discovery_budget_exhausted:
    return diagnostic.direct.unresolved_reason.empty() && !diagnostic.direct.has_target
               ? std::optional{GenesisFrontierClass::discovery_prefix_boundary}
               : std::nullopt;
  default:
    return std::nullopt;
  }
}

} // namespace

std::vector<GenesisLogicalTableDescriptorHint> parse_genesis_external_hints(std::string_view json_text,
                                                                             std::string_view expected_rom_sha256) {
  std::vector<GenesisLogicalTableDescriptorHint> out;
  JsonParser parser(json_text);
  const auto root = parser.parse();
  if (!root) return out;  // whole-file JSON syntax failure: fail closed to zero hints
  std::vector<const JsonValue *> candidates;
  if (root->kind == JsonValue::Kind::array) {
    for (const auto &item : root->array_value) candidates.push_back(&item);
  } else if (root->kind == JsonValue::Kind::object) {
    candidates.push_back(&*root);
  } else {
    return out;  // a bare string/number/bool/null top level carries no hint record
  }
  for (const auto *candidate : candidates)
    if (auto hint = parse_one_external_hint(*candidate, expected_rom_sha256)) out.push_back(std::move(*hint));

  // SEG-007-T164 hardening: reject silent conflict resolution. Two or more
  // accepted (parsed, ROM-hash-verified) records that share the same logical
  // table identity `(base_address, entry_width_bytes)` must agree on every
  // semantic field (`stride_bytes`, `entry_count`); provenance fields may
  // differ freely since provenance is metadata, not semantic content.
  // Identical duplicates are harmless and are kept as-is (the consumer only
  // ever needs one match). Any real disagreement means this table's logical
  // identity is genuinely ambiguous, so every record for that identity is
  // dropped -- `logical_table_descriptor_hint` then returns `std::nullopt`
  // for that `(base_address, entry_width_bytes)`, exactly like the
  // not-opted-in/no-match case, rather than an arbitrary "first one wins".
  std::vector<GenesisLogicalTableDescriptorHint> resolved;
  resolved.reserve(out.size());
  for (std::size_t i = 0; i < out.size(); ++i) {
    bool conflict = false;
    for (std::size_t j = 0; j < out.size(); ++j) {
      if (i == j) continue;
      if (out[j].base_address != out[i].base_address || out[j].entry_width_bytes != out[i].entry_width_bytes)
        continue;
      if (out[j].stride_bytes != out[i].stride_bytes || out[j].entry_count != out[i].entry_count) {
        conflict = true;
        break;
      }
    }
    if (!conflict) resolved.push_back(out[i]);
  }
  return resolved;
}

std::vector<GenesisCodeEntryCandidateHint> parse_genesis_external_code_entry_candidates(
    std::string_view json_text, std::string_view expected_rom_sha256) {
  std::vector<GenesisCodeEntryCandidateHint> out;
  JsonParser parser(json_text);
  const auto root = parser.parse();
  if (!root) return out;  // whole-file JSON syntax failure: fail closed to zero candidates
  std::vector<const JsonValue *> candidates;
  if (root->kind == JsonValue::Kind::array) {
    for (const auto &item : root->array_value) candidates.push_back(&item);
  } else if (root->kind == JsonValue::Kind::object) {
    candidates.push_back(&*root);
  } else {
    return out;  // a bare string/number/bool/null top level carries no candidate record
  }
  for (const auto *candidate : candidates)
    if (auto hint = parse_one_code_entry_candidate(*candidate, expected_rom_sha256)) out.push_back(std::move(*hint));

  // SEG-007-T174 / ADR-0024 correction: deterministic sort+dedup by address.
  // Unlike ADR-0023's logical-table-descriptor records, a candidate carries
  // no semantic content beyond "this address is worth an independent walk",
  // so two records for the same address are never a conflict -- exactly one
  // survives. The survivor selection itself must be a deterministic,
  // input-order-independent function of the records' OWN content, not of
  // whichever order they happened to appear in the source file:
  // `std::sort`'s ordering among elements the comparator treats as equal
  // (same address) is unspecified, so sorting on address alone would leave
  // the tie-break among same-address duplicates dependent on the sort
  // algorithm's own behavior for equal keys, not on the input. Instead, sort
  // by the FULL lexicographic tuple (address, tool, tool_version, timestamp,
  // human_reviewed) -- an explicit, documented total order over every field
  // -- so two hints files containing the same candidates in different
  // record order, including differing only in which duplicate-at-the-same-
  // address record appears first in the file, produce byte-identical parsed
  // results. `std::unique` then keeps the first survivor (address-equal
  // tuples are already ordered by the same explicit total order) and drops
  // the rest as exact duplicates of the same seed.
  std::sort(out.begin(), out.end(),
            [](const GenesisCodeEntryCandidateHint &a, const GenesisCodeEntryCandidateHint &b) {
              return std::tie(a.address, a.provenance_tool, a.provenance_tool_version,
                               a.provenance_timestamp, a.provenance_human_reviewed) <
                     std::tie(b.address, b.provenance_tool, b.provenance_tool_version,
                              b.provenance_timestamp, b.provenance_human_reviewed);
            });
  out.erase(std::unique(out.begin(), out.end(),
                        [](const GenesisCodeEntryCandidateHint &a, const GenesisCodeEntryCandidateHint &b) {
                          return a.address == b.address;
                        }),
            out.end());
  return out;
}

std::vector<GenesisCodePointerTableDescriptorHint>
parse_genesis_external_code_pointer_table_descriptors(
    std::string_view json_text, std::string_view expected_rom_sha256) {
  std::vector<GenesisCodePointerTableDescriptorHint> parsed;
  JsonParser parser(json_text);
  const auto root = parser.parse();
  if (!root) return parsed;
  std::vector<const JsonValue *> records;
  if (root->kind == JsonValue::Kind::array) {
    for (const auto &item : root->array_value) records.push_back(&item);
  } else if (root->kind == JsonValue::Kind::object) {
    records.push_back(&*root);
  } else {
    return parsed;
  }
  for (const auto *record : records)
    if (auto descriptor = parse_one_code_pointer_table_descriptor(*record, expected_rom_sha256))
      parsed.push_back(std::move(*descriptor));

  std::vector<GenesisCodePointerTableDescriptorHint> resolved;
  resolved.reserve(parsed.size());
  for (std::size_t i = 0; i < parsed.size(); ++i) {
    bool conflict = false;
    for (std::size_t j = 0; j < parsed.size(); ++j) {
      if (i != j && parsed[i].base_address == parsed[j].base_address &&
          parsed[i].entry_count != parsed[j].entry_count) {
        conflict = true;
        break;
      }
    }
    if (!conflict) resolved.push_back(parsed[i]);
  }
  std::sort(resolved.begin(), resolved.end(),
            [](const auto &a, const auto &b) {
              return std::tie(a.base_address, a.entry_count, a.provenance_tool,
                              a.provenance_tool_version, a.provenance_timestamp) <
                     std::tie(b.base_address, b.entry_count, b.provenance_tool,
                              b.provenance_tool_version, b.provenance_timestamp);
            });
  return resolved;
}

// SEG-007-T204 / ADR-0033.
std::vector<GenesisAddressTableCandidateHint> parse_genesis_external_address_table_candidates(
    std::string_view json_text, std::string_view expected_rom_sha256) {
  std::vector<GenesisAddressTableCandidateHint> parsed;
  JsonParser parser(json_text);
  const auto root = parser.parse();
  if (!root) return parsed;
  std::vector<const JsonValue *> records;
  if (root->kind == JsonValue::Kind::array) {
    for (const auto &item : root->array_value) records.push_back(&item);
  } else if (root->kind == JsonValue::Kind::object) {
    records.push_back(&*root);
  } else {
    return parsed;
  }
  for (const auto *record : records)
    if (auto candidate = parse_one_address_table_candidate(*record, expected_rom_sha256))
      parsed.push_back(std::move(*candidate));

  std::vector<GenesisAddressTableCandidateHint> resolved;
  resolved.reserve(parsed.size());
  for (std::size_t i = 0; i < parsed.size(); ++i) {
    bool conflict = false;
    for (std::size_t j = 0; j < parsed.size(); ++j) {
      if (i != j && parsed[i].base_address == parsed[j].base_address &&
          parsed[i].entry_count != parsed[j].entry_count) {
        conflict = true;
        break;
      }
    }
    if (!conflict) resolved.push_back(parsed[i]);
  }
  std::sort(resolved.begin(), resolved.end(),
            [](const auto &a, const auto &b) {
              return std::tie(a.base_address, a.entry_count, a.provenance_tool,
                              a.provenance_tool_version, a.provenance_timestamp) <
                     std::tie(b.base_address, b.entry_count, b.provenance_tool,
                              b.provenance_tool_version, b.provenance_timestamp);
            });
  return resolved;
}

// SEG-007-T206: mechanical, non-authoritative proposal detection. See the
// header's own doc comment for the full contract. This lives in the
// Genesis-specific frontend layer (never `static_discovery.cpp`, which
// stays CPU-generic) -- the same layering `apply_genesis_address_table_
// corroboration` (ADR-0033) already established for the 4-byte case.
std::vector<GenesisPcRelativeOffsetTableExtentProposalHint>
detect_genesis_pc_relative_offset_table_extent_proposals(
    const std::vector<M68kUnprovenIndirectControlEaSet> &unproven_indirect_control_ea_sets,
    const std::vector<GenesisLogicalTableDescriptorHint> &existing_descriptors,
    std::string provenance_tool, std::string provenance_tool_version, std::string provenance_timestamp) {
  std::vector<GenesisPcRelativeOffsetTableExtentProposalHint> out;
  std::set<std::uint32_t> seen_bases;
  for (const auto &unproven : unproven_indirect_control_ea_sets) {
    const auto &ea = unproven.control_ea;
    // Only the recognized word-Dn-indexed shape both `m68k_fold_immutable_
    // offset_table` and `compute_indirect_target_set` already restrict
    // themselves to; never An-indexed or long-sized (a genuinely different
    // EA shape neither this proposal nor either existing consumer covers).
    if (ea.index_is_address || ea.index_is_long) continue;
    const auto raw =
        static_cast<std::int64_t>(ea.pc_base_address) + static_cast<std::int64_t>(ea.displacement);
    if (raw < 0 || raw > static_cast<std::int64_t>(UINT32_C(0x00FFFFFF))) continue;  // non-24-bit / overflow
    const auto base_address = static_cast<std::uint32_t>(raw);
    if (!seen_bases.insert(base_address).second) continue;  // already proposed this base once
    const bool already_described =
        std::any_of(existing_descriptors.begin(), existing_descriptors.end(),
                    [&](const GenesisLogicalTableDescriptorHint &descriptor) {
                      return descriptor.base_address == base_address && descriptor.entry_width_bytes == 2U;
                    });
    if (already_described) continue;
    out.push_back({base_address, 2U, 2U, provenance_tool, provenance_tool_version, provenance_timestamp, false});
  }
  std::sort(out.begin(), out.end(),
            [](const GenesisPcRelativeOffsetTableExtentProposalHint &a,
               const GenesisPcRelativeOffsetTableExtentProposalHint &b) {
              return a.base_address < b.base_address;
            });
  return out;
}

namespace {
// Minimal string escaper for the small, fixed vocabulary of provenance
// strings this exporter ever writes (tool/tool_version/timestamp -- never
// arbitrary untrusted input); mirrors the narrow escaping every other
// hand-written JSON emitter in this file already performs.
std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2U);
  for (const char character : text) {
    switch (character) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    default: out += character; break;
    }
  }
  return out;
}
} // namespace

std::string serialize_genesis_pc_relative_offset_table_extent_proposals(
    const std::vector<GenesisPcRelativeOffsetTableExtentProposalHint> &proposals, std::string_view rom_sha256) {
  std::string out = "[";
  bool first = true;
  for (const auto &proposal : proposals) {
    if (!first) out += ",";
    first = false;
    out += "{\"kind\":\"pc_relative_offset_table_extent_proposal\",\"rom_sha256\":\"";
    out += json_escape(rom_sha256);
    out += "\",\"base_address\":";
    out += std::to_string(proposal.base_address);
    out += ",\"entry_width_bytes\":";
    out += std::to_string(proposal.entry_width_bytes);
    out += ",\"stride_bytes\":";
    out += std::to_string(proposal.stride_bytes);
    out += ",\"provenance\":{\"tool\":\"";
    out += json_escape(proposal.provenance_tool);
    out += "\",\"tool_version\":\"";
    out += json_escape(proposal.provenance_tool_version);
    out += "\",\"timestamp\":\"";
    out += json_escape(proposal.provenance_timestamp);
    out += "\",\"human_reviewed\":";
    out += proposal.provenance_human_reviewed ? "true" : "false";
    out += "}}";
  }
  out += "]\n";
  return out;
}

namespace {
// Shared entry-extraction/candidate-emission primitive reused by BOTH
// `apply_genesis_code_pointer_table_descriptors` (ADR-0032, human-reviewed
// manual descriptors) and `apply_genesis_address_table_corroboration`
// (ADR-0033, machine-corroborated candidates), so the two descriptor
// promotion paths cannot silently drift on how a `[base_address,
// base_address + 4 * entry_count)` interval is resolved to a single
// unambiguous, structurally valid `raw_cartridge_rom` mapping claim and read.
// Every extracted 4-byte big-endian value is appended ONLY as an ordinary
// `code_entry_candidate` proposal (`provenance_human_reviewed` as supplied by
// the caller) -- never granted direct emitted-code authority. Returns
// `false` (and appends nothing) if the interval does not resolve to exactly
// one covering, structurally valid `raw_cartridge_rom` claim.
bool append_immutable_code_pointer_table_entries_as_candidates(
    FrontendProgram &program, std::uint32_t base_address, std::uint32_t entry_count,
    const std::string &provenance_tool, const std::string &provenance_tool_version,
    const std::string &provenance_timestamp, bool provenance_human_reviewed) {
  const std::uint64_t span = static_cast<std::uint64_t>(entry_count) * 4U;
  const std::uint64_t end = static_cast<std::uint64_t>(base_address) + span;
  if (end > UINT64_C(0x1'0000'0000)) return false;
  const MappingClaim *covering = nullptr;
  bool ambiguous = false;
  for (const auto &claim : program.mapping_claims) {
    if (claim.target_begin.space != TargetAddressSpace::m68k_program ||
        claim.target_end.space != TargetAddressSpace::m68k_program ||
        !(base_address < claim.target_end.value && claim.target_begin.value < end))
      continue;
    if (covering != nullptr) {
      ambiguous = true;
      break;
    }
    covering = &claim;
  }
  if (ambiguous || covering == nullptr || covering->name != "raw_cartridge_rom" ||
      !structurally_valid_mapping_claim(*covering) ||
      base_address < covering->target_begin.value || end > covering->target_end.value)
    return false;
  const std::uint64_t local = static_cast<std::uint64_t>(base_address) - covering->target_begin.value;
  if (local > std::numeric_limits<std::uint64_t>::max() - covering->image_begin.value) return false;
  const std::uint64_t image_begin = covering->image_begin.value + local;
  if (span > std::numeric_limits<std::uint64_t>::max() - image_begin) return false;
  const std::uint64_t image_end = image_begin + span;
  if (image_end > covering->image_end.value || image_end > program.image.bytes.size()) return false;
  for (std::uint32_t i = 0; i < entry_count; ++i) {
    const std::size_t offset = static_cast<std::size_t>(image_begin + static_cast<std::uint64_t>(i) * 4U);
    const std::uint32_t value = (static_cast<std::uint32_t>(program.image.bytes[offset]) << 24U) |
                                (static_cast<std::uint32_t>(program.image.bytes[offset + 1U]) << 16U) |
                                (static_cast<std::uint32_t>(program.image.bytes[offset + 2U]) << 8U) |
                                static_cast<std::uint32_t>(program.image.bytes[offset + 3U]);
    program.external_code_entry_candidates.push_back(
        {value, provenance_tool, provenance_tool_version, provenance_timestamp, provenance_human_reviewed});
  }
  return true;
}

// Deterministic sort+dedup shared by both descriptor-promotion paths above,
// so either one can be called standalone (as every unit test does) and still
// leave `external_code_entry_candidates` in the same canonical order/dedup
// state the combined pipeline (main.cpp) produces.
void sort_and_dedup_external_code_entry_candidates(FrontendProgram &program) {
  auto &candidates = program.external_code_entry_candidates;
  std::sort(candidates.begin(), candidates.end(),
            [](const auto &a, const auto &b) {
              return std::tie(a.address, a.provenance_tool, a.provenance_tool_version,
                              a.provenance_timestamp, a.provenance_human_reviewed) <
                     std::tie(b.address, b.provenance_tool, b.provenance_tool_version,
                              b.provenance_timestamp, b.provenance_human_reviewed);
            });
  candidates.erase(std::unique(candidates.begin(), candidates.end(),
                               [](const auto &a, const auto &b) { return a.address == b.address; }),
                   candidates.end());
}
} // namespace

void apply_genesis_code_pointer_table_descriptors(FrontendProgram &program) {
  for (const auto &descriptor : program.external_code_pointer_table_descriptors) {
    if (descriptor.entry_count == 0U ||
        descriptor.entry_count > genesis_code_pointer_table_max_entries ||
        descriptor.provenance_tool.empty() || descriptor.provenance_tool_version.empty() ||
        descriptor.provenance_timestamp.empty() || !descriptor.provenance_human_reviewed)
      continue;
    append_immutable_code_pointer_table_entries_as_candidates(
        program, descriptor.base_address, descriptor.entry_count, descriptor.provenance_tool,
        descriptor.provenance_tool_version, descriptor.provenance_timestamp, true);
  }
  sort_and_dedup_external_code_entry_candidates(program);
}

bool apply_genesis_immutable_rom_aot_range(FrontendProgram &program, std::uint32_t begin_address,
                                           std::uint32_t end_address) {
  if (end_address <= begin_address || (begin_address & 1U) != 0U || (end_address & 1U) != 0U) return false;
  const std::uint64_t span = static_cast<std::uint64_t>(end_address) - begin_address;
  const std::uint64_t candidate_count = span / 2U;
  if (candidate_count == 0U || candidate_count > genesis_test_immutable_rom_aot_range_max_candidates) return false;
  // Resolve the whole interval to exactly one structurally valid
  // `raw_cartridge_rom` mapping claim -- the identical resolution rule
  // `append_immutable_code_pointer_table_entries_as_candidates` above uses,
  // so this experiment can never reach outside bounded mapped immutable
  // M68k code storage.
  const MappingClaim *covering = nullptr;
  bool ambiguous = false;
  for (const auto &claim : program.mapping_claims) {
    if (claim.target_begin.space != TargetAddressSpace::m68k_program ||
        claim.target_end.space != TargetAddressSpace::m68k_program ||
        !(begin_address < claim.target_end.value && claim.target_begin.value < end_address))
      continue;
    if (covering != nullptr) {
      ambiguous = true;
      break;
    }
    covering = &claim;
  }
  if (ambiguous || covering == nullptr || covering->name != "raw_cartridge_rom" ||
      !structurally_valid_mapping_claim(*covering) ||
      begin_address < covering->target_begin.value || end_address > covering->target_end.value)
    return false;
  const std::uint64_t local_begin = static_cast<std::uint64_t>(begin_address) - covering->target_begin.value;
  if (local_begin > std::numeric_limits<std::uint64_t>::max() - covering->image_begin.value) return false;
  const std::uint64_t image_begin = covering->image_begin.value + local_begin;
  if (span > std::numeric_limits<std::uint64_t>::max() - image_begin) return false;
  const std::uint64_t image_end = image_begin + span;
  if (image_end > covering->image_end.value || image_end > program.image.bytes.size()) return false;
  program.immutable_rom_aot_ranges.push_back({begin_address, end_address});
  std::sort(program.immutable_rom_aot_ranges.begin(), program.immutable_rom_aot_ranges.end(),
            [](const auto &left, const auto &right) {
              return std::tie(left.begin_address, left.end_address) <
                     std::tie(right.begin_address, right.end_address);
            });
  program.immutable_rom_aot_ranges.erase(
      std::unique(program.immutable_rom_aot_ranges.begin(), program.immutable_rom_aot_ranges.end(),
                  [](const auto &left, const auto &right) {
                    return left.begin_address == right.begin_address && left.end_address == right.end_address;
                  }),
      program.immutable_rom_aot_ranges.end());
  return true;
}

bool apply_genesis_immutable_rom_aot(FrontendProgram &program) {
  std::vector<FrontendProgram::ImmutableRomAotRange> ranges;
  for (const auto &claim : program.mapping_claims) {
    if (claim.name != "raw_cartridge_rom") continue;
    if (!structurally_valid_mapping_claim(claim) || claim.image_end.value > program.image.bytes.size()) return false;
    for (const auto &other : program.mapping_claims) {
      if (&other == &claim) continue;
      if (!structurally_valid_mapping_claim(other)) return false;
      if (claim.target_begin.value < other.target_end.value &&
          other.target_begin.value < claim.target_end.value)
        return false;
    }
    const auto begin = static_cast<std::uint32_t>((claim.target_begin.value + 1U) & ~UINT32_C(1));
    if (begin < claim.target_end.value) ranges.push_back({begin, claim.target_end.value});
  }
  if (ranges.empty()) return false;
  std::sort(ranges.begin(), ranges.end(), [](const auto &left, const auto &right) {
    return std::tie(left.begin_address, left.end_address) < std::tie(right.begin_address, right.end_address);
  });
  program.immutable_rom_aot_ranges = std::move(ranges);
  program.immutable_rom_aot_enabled = true;
  return true;
}

// SEG-007-T204 / ADR-0033 (re-homed by the 2026-09-11 operator correction):
// promotes `external_address_table_candidates` into ordinary
// `external_code_entry_candidates` PROPOSALS -- never directly into
// ADR-0023's `external_logical_table_descriptor_hints` trust path -- but
// only when two independently-sourced boundary signals exactly agree. See
// ADR-0033 for the complete design and rationale (in particular why this
// defeats T164's prior single-source over-approximation failure, and why a
// promoted candidate is still just an ordinary ADR-0025 proposal, not a
// trusted finite-selector-domain fact: ADR-0023's own consumer,
// `M68kGeneralStartupEnvironment::logical_table_descriptor_hint`, is only
// ever looked up for a 2-byte PC-relative offset-word table shape, which
// this 4-byte absolute-code-pointer mechanism structurally cannot produce).
// `apply_genesis_code_pointer_table_descriptors`'s own manual descriptor
// path, `external_logical_table_descriptor_hints`, and every existing
// structural proof downstream of it are completely unchanged by this
// function -- it never reads or writes that vector.
void apply_genesis_address_table_corroboration(FrontendProgram &program) {
  for (const auto &candidate : program.external_address_table_candidates) {
    if (candidate.entry_width_bytes != 4U || candidate.stride_bytes != 4U ||
        candidate.entry_count == 0U || candidate.entry_count > genesis_code_pointer_table_max_entries ||
        candidate.provenance_tool.empty() || candidate.provenance_tool_version.empty() ||
        candidate.provenance_timestamp.empty())
      continue;
    // Misaligned/odd table storage base is rejected outright: never a
    // plausible table of code pointers, and not worth resolving further.
    if ((candidate.base_address & 1U) != 0U) continue;

    // Resolve the candidate's own base address to a single unambiguous,
    // structurally valid `raw_cartridge_rom` mapping claim -- the same rule
    // `apply_genesis_code_pointer_table_descriptors` applies, evaluated here
    // against a single 4-byte probe first (the full extent is not yet known:
    // that is exactly the fact this walk is about to independently derive).
    const std::uint64_t probe_end = static_cast<std::uint64_t>(candidate.base_address) + 4U;
    const MappingClaim *covering = nullptr;
    bool ambiguous = false;
    for (const auto &claim : program.mapping_claims) {
      if (claim.target_begin.space != TargetAddressSpace::m68k_program ||
          claim.target_end.space != TargetAddressSpace::m68k_program ||
          !(candidate.base_address < claim.target_end.value && claim.target_begin.value < probe_end))
        continue;
      if (covering != nullptr) {
        ambiguous = true;
        break;
      }
      covering = &claim;
    }
    if (ambiguous || covering == nullptr || covering->name != "raw_cartridge_rom" ||
        !structurally_valid_mapping_claim(*covering) || candidate.base_address < covering->target_begin.value)
      continue;
    const std::uint64_t local = static_cast<std::uint64_t>(candidate.base_address) - covering->target_begin.value;
    if (local > std::numeric_limits<std::uint64_t>::max() - covering->image_begin.value) continue;
    const std::uint64_t base_image_offset = covering->image_begin.value + local;
    const std::uint64_t claim_image_end =
        std::min<std::uint64_t>(covering->image_end.value, program.image.bytes.size());
    if (base_image_offset > claim_image_end) continue;

    // Signal B: the self-terminating prefix walk. Independently re-derives
    // where the table ends using only the EXISTING, unweakened per-entry
    // ADR-0025 call-target validation (`validate_m68k_static_call_target`) --
    // never a new or relaxed rule. Bounded by the same fixed schema cap as
    // every other table-shaped descriptor in this file, plus one extra
    // lookahead entry so a Ghidra under-approximation (a valid entry
    // immediately past Ghidra's own claimed boundary) is itself detectable
    // as a disagreement rather than silently accepted.
    constexpr std::uint32_t walk_cap = genesis_code_pointer_table_max_entries + 1U;
    std::uint32_t self_terminating_count = 0U;
    for (; self_terminating_count < walk_cap; ++self_terminating_count) {
      const std::uint64_t entry_offset =
          base_image_offset + static_cast<std::uint64_t>(self_terminating_count) * 4U;
      if (entry_offset + 4U > claim_image_end) break;
      const std::size_t offset = static_cast<std::size_t>(entry_offset);
      const std::uint32_t value = (static_cast<std::uint32_t>(program.image.bytes[offset]) << 24U) |
                                  (static_cast<std::uint32_t>(program.image.bytes[offset + 1U]) << 16U) |
                                  (static_cast<std::uint32_t>(program.image.bytes[offset + 2U]) << 8U) |
                                  static_cast<std::uint32_t>(program.image.bytes[offset + 3U]);
      if (validate_m68k_static_call_target(program, value)) break;
    }

    // Signal A vs. signal B: promotion requires EXACT agreement. Either
    // direction of disagreement (Ghidra's raw analyzer over- or under-
    // approximated relative to this independent per-entry walk) fails
    // closed -- no proposals, never a guessed count.
    //
    // IMPORTANT trust-boundary caveat (see ADR-0033 "Trust boundary"): exact
    // agreement between these two signals is NOT a universal proof of table
    // identity. It only proves that (a) Ghidra's own array/pointer-density
    // heuristic and (b) this project's own per-entry absolute-pointer
    // structural check land on the identical boundary. A neighboring data
    // value can coincidentally satisfy `validate_m68k_static_call_target`
    // (24-bit-clean, even, uniquely mapped) without actually being a code
    // pointer, and a genuine table can sit immediately adjacent to another,
    // unrelated, valid-looking pointer region. Every extracted value below
    // is therefore appended ONLY as an ordinary `code_entry_candidate`
    // proposal -- it still must independently survive the complete,
    // unweakened ADR-0025 admission walk (decode, discovery, retention,
    // emission) before it can ever affect generated output.
    if (self_terminating_count != candidate.entry_count) continue;

    append_immutable_code_pointer_table_entries_as_candidates(
        program, candidate.base_address, candidate.entry_count, candidate.provenance_tool,
        candidate.provenance_tool_version, candidate.provenance_timestamp, false);
  }
  sort_and_dedup_external_code_entry_candidates(program);
}

StaticCallReturnResult discover_m68k_static_call_return(const FrontendProgram &program,
                                                          const M68kDecodedInstruction &call_decoded,
                                                          const M68kDecodedInstruction &return_decoded) {
  if (call_decoded.kind != M68kInstructionKind::jsr) {
    auto r = rejected(DirectFlowDiagnostic::startup_graph_mismatch, program);
    set_source(r, call_decoded.provenance.source.address);
    r.image_offset = call_decoded.provenance.source.image_offset;
    r.provenance = call_decoded.provenance;
    r.direct.provenance = r.provenance.value();
    r.direct.has_provenance = true;
    r.instruction_length = call_decoded.provenance.length.value;
    return r;
  }
  if (!m68k_is_statically_foldable_control_ea(call_decoded.source_ea)) {
    auto r = rejected(DirectFlowDiagnostic::reached_unresolved_direct_edge, program);
    set_source(r, call_decoded.provenance.source.address);
    r.image_offset = call_decoded.provenance.source.image_offset;
    r.provenance = call_decoded.provenance;
    r.direct.provenance = r.provenance.value();
    r.direct.has_provenance = true;
    r.instruction_length = call_decoded.provenance.length.value;
    return r;
  }
  const auto target = m68k_canonical_ea_address(call_decoded.source_ea);
  if (const auto failure = validate_m68k_static_call_target(program, target))
    return build_static_call_target_rejection(program, call_decoded, *failure, target);
  if (return_decoded.kind != M68kInstructionKind::rts) {
    auto r = rejected(DirectFlowDiagnostic::startup_graph_mismatch, program);
    set_source(r, return_decoded.provenance.source.address);
    r.image_offset = return_decoded.provenance.source.image_offset;
    r.provenance = return_decoded.provenance;
    r.direct.provenance = r.provenance.value();
    r.direct.has_provenance = true;
    r.instruction_length = return_decoded.provenance.length.value;
    return r;
  }
  const auto call = m68k_make_static_call(call_decoded, target);
  if (!same(return_decoded.provenance.source.address, call.callee)) {
    auto r = rejected(DirectFlowDiagnostic::return_target_mismatch, program);
    set_source(r, return_decoded.provenance.source.address);
    r.image_offset = return_decoded.provenance.source.image_offset;
    r.provenance = return_decoded.provenance;
    r.direct.provenance = r.provenance.value();
    r.direct.has_provenance = true;
    r.instruction_length = return_decoded.provenance.length.value;
    r.direct.target = call.callee;
    r.direct.has_target = true;
    return r;
  }
  const auto call_edge = m68k_make_static_call_edge(call);
  const auto return_edge = m68k_make_static_return_edge(call, return_decoded);
  const M68kStaticFrame frame{call};
  return StaticCallReturnDiscovery{call, call_edge, return_edge, frame};
}

// Bounded general static discovery (SEG-007-T010). See the header declaration
// for the contract; this reuses the same claim-lookup, decode-translation,
// absolute-operand policy, and call/return identity helpers already defined
// above (claims/rejected/set_source/set_claims/validate_m68k_static_call_target/
// build_static_call_target_rejection/build_m68k_static_call/
// m68k_make_static_call_edge/m68k_make_static_return_edge) rather than
// duplicating any of their logic. It never reads from or writes to
// DirectFlowAnalysis, and never runs the fixed five-operation
// analyze_startup_profile route below.
//
// Two structures deliberately serve two different identities (SEG-007-T010,
// third correction pass):
//   - decoded_by_address is the sole canonical instruction/IR-decode-
//     ownership structure, keyed by SOURCE ADDRESS ONLY. An address is
//     decoded at most once, ever, regardless of how many control-flow paths
//     or how many distinct static call-frame contexts reach it.
//   - visited_states is a separate TRAVERSAL-VISITATION structure, keyed by
//     (source address, active static call-frame context). It -- not
//     decode-cache presence in decoded_by_address -- is what determines
//     whether a given control-flow path has "nothing more to do" at a given
//     address: an RTS's meaning (which continuation it returns to) depends
//     on which call is currently open, not on the RTS's address alone, so
//     the same already-decoded instruction (most importantly a shared
//     callee's body and its RTS) can be legitimately processed under more
//     than one traversal state, producing more than one contextual graph
//     fact -- e.g. more than one return_to_continuation edge from the same
//     RTS address, one per distinct calling static call site. Only when the
//     exact same (address, call-stack) pair is reached a second time is the
//     traversal genuinely redundant.
// Because a decoded instruction can now be processed under more than one
// traversal state, the static graph facts it contributes (branch edges, a
// JSR's own call/frame, an RTS's return edge) are deduplicated separately by
// their own natural identity -- never implicitly deduplicated by
// decoded_by_address presence -- via branch_edges_emitted/calls_emitted
// (keyed by the instruction's own source address, since those facts never
// vary by calling context) and return_edges_emitted (keyed by (RTS source
// address, call-site identity), since a return edge's target does depend on
// which call is active).
FrontendResult discover_m68k_general_startup(const FrontendProgram &program) {
  if (!program.startup_ingress) return rejected(DirectFlowDiagnostic::unmapped_instruction_address, program);
  const auto entry = program.startup_ingress->entry;
  if (entry.space != TargetAddressSpace::m68k_program)
    return rejected(DirectFlowDiagnostic::valid_but_unsupported_instruction, program);
  if ((entry.value & 1U) != 0U) {
    auto r = rejected(DirectFlowDiagnostic::odd_instruction_address, program);
    set_source(r, entry);
    return r;
  }

  FrontendAnalysis analysis{};
  analysis.profile = M68kFrontendProfile::general_startup;
  analysis.mapping_claims = program.mapping_claims;
  analysis.startup_ingress = program.startup_ingress;

  // SEG-014-T003: the CPU-owned production static-graph traversal
  // (discover_m68k_static_graph, libs/cpu/m68k/src/static_discovery.cpp) now owns
  // the recursive DFS + `switch (decoded.kind)` successor selector this
  // function used to run directly. This scenario adapter supplies only the
  // narrow M68kStaticDiscoveryEnvironment fact boundary (mapping/decode
  // source admission, direct-target admission, memory-operand
  // classification, and synthetic-completion recognition) below and then
  // translates the returned graph/issue facts back into this profile's own
  // established FrontendAnalysis/FrontendRejected shapes -- it never
  // re-implements any traversal decision itself.
  M68kGeneralStartupEnvironment environment(program);
  const M68kStaticDiscoveryLimits limits{m68k_discovery_max_instructions, m68k_discovery_max_blocks,
                                          m68k_discovery_max_call_frame_depth};

  // ADR-0013 Decision §7 Phase B / ADR-0011 §4.1: walk each seed (round 1's
  // fixed entry plus any driver-supplied runtime_confirmed seeds -- see
  // FrontendProgram::runtime_confirmed_seeds) independently, then
  // deterministically aggregate and dedup the per-walk facts below in seed
  // order, then per-walk first-recognition order. A single seed (the default,
  // unchanged shape every existing caller uses) produces byte-identical
  // results to the pre-Phase-B single-walk code this replaces. Edges and call
  // frames legitimately support more than one fact sharing a source/caller
  // address already within a SINGLE walk (an RTS shared by more than one
  // call site, or an ADR-0009 indirect site's own multiple proven
  // candidates), so those are deduplicated by their own full, exact
  // signature only -- never flagged as a conflict merely for sharing a
  // source address. Two walks disagreeing about the same address's decoded
  // instruction, indirect target-EA candidate set, or completion RTS fail
  // the whole translation closed rather than silently choosing one -- the
  // environment is a pure function of already-validated program state, so
  // this is a defensive invariant, never an expected outcome of a
  // legitimate seed set.
  // SEG-007-T174 / ADR-0024 Decision §4: the seed set is now
  // `{reset entry} ∪ external_code_entry_candidates (Ghidra, or equivalent,
  // proposals) ∪ runtime_confirmed_seeds (Phase B, demoted to fallback)`.
  // `external_code_entry_candidates` is empty by default (ordinary raw-ROM
  // recompilation that never opts into `--external-hints` never populates
  // it), so this reduces to the prior `{reset entry} ∪
  // runtime_confirmed_seeds` byte-for-byte for every existing caller. Every
  // seed, from whichever source, still receives its own fully independent
  // per-root walk through the unchanged discovery machinery below -- folding
  // in a candidate here grants it no special authority.
  std::vector<Address> seeds{entry.value};
  for (const auto &candidate : program.external_code_entry_candidates)
    if (std::find(seeds.begin(), seeds.end(), candidate.address) == seeds.end()) seeds.push_back(candidate.address);
  for (const auto &seed : program.runtime_confirmed_seeds)
    if (std::find(seeds.begin(), seeds.end(), seed.value) == seeds.end()) seeds.push_back(seed.value);
  // ADR-0013 Decision §7c: `m68k_discovery_max_seed_entries` bounds only the
  // Phase B driver's own count of NEWLY PROMOTED runtime-confirmed roots in
  // ONE driver invocation (enforced entirely in
  // tools/genesis_startup_bridge.py's run_expansion_loop). It is not, and
  // must not become, a total-cardinality ceiling on the persisted confirmed-
  // root set this function receives: `program.runtime_confirmed_seeds` is a
  // flat list that carries no distinction between "persisted from an earlier
  // invocation" and "newly promoted this invocation", so this frontend has no
  // way to re-derive that per-invocation count from the flat list alone and
  // must not attempt to re-enforce it as a total-count bound here. A
  // checkpoint/resume driver invocation legitimately hands this function any
  // number of previously-confirmed roots (all of Decision §7c's persistent
  // set) plus at most m68k_discovery_max_seed_entries newly promoted ones;
  // every one of those seeds still gets its own fully independent,
  // per-root-bounded walk below (m68k_discovery_max_instructions), so no
  // safety invariant depends on the total seed count. Every other existing
  // resource/safety ceiling below (the per-root 256U instruction walk limit,
  // m68k_discovery_max_blocks, call-depth, dedup, the aggregation_conflict
  // fail-closed cross-root disagreement path) is unaffected and unweakened.
  std::map<Address, M68kDecodedInstruction> merged_decoded;
  std::vector<Address> decode_order;
  std::set<Address> block_entries;
  std::vector<Address> block_entry_order;
  // Edge signature: (source address, kind, target address, has-call,
  // callee, continuation). A source instruction can legitimately produce
  // more than one edge sharing a (source, kind) pair -- e.g. an RTS shared
  // by more than one call site emits one return_to_continuation edge per
  // distinct caller (M68kDiscoveryIssue's own doc comments), and an
  // ADR-0009 indirect edge emits one edge per proven candidate target -- so
  // only an EXACT duplicate signature is deduplicated; no narrower key is a
  // conflict.
  using EdgeSignature = std::tuple<Address, int, Address, bool, Address, Address>;
  std::set<EdgeSignature> edge_signatures_seen;
  std::vector<M68kStaticEdge> merged_edges;
  std::set<std::tuple<Address, Address, Address>> frame_signatures_seen;
  std::vector<M68kStaticFrame> merged_frames;
  std::map<Address, M68kIndirectTargetEaSet> indirect_by_source;
  std::vector<Address> indirect_order;
  // SEG-007-T174 / ADR-0024: the weaker Tier-2-eligible sibling map, merged
  // identically in shape to `indirect_by_source` above (a source address
  // never legitimately produces two disagreeing unproven facts across
  // independent per-root walks, since the environment is a pure function of
  // already-validated program state).
  std::map<Address, M68kUnprovenIndirectControlEaSet> unproven_indirect_by_source;
  std::vector<Address> unproven_indirect_order;
  std::optional<InstructionProvenance> completion_rts;
  std::optional<M68kDiscoveryIssue> primary_issue;
  // SEG-007-T213: the originating root for `primary_issue`, tracked in
  // parallel exactly like `aggregated_secondary_issue_roots` tracks each
  // secondary issue's root, so the cross-root admitted-address supersession
  // predicate can be applied to `primary_issue` with the same evidence it
  // already uses for every secondary issue.
  std::optional<StaticProgramRootId> primary_issue_root;
  std::vector<M68kDiscoveryIssue> aggregated_secondary_issues;
  // ADR-0021 §2a: the originating StaticProgramRootId for every entry pushed
  // into aggregated_secondary_issues (kept exactly parallel to it), plus,
  // per root (synchronous seed or the single asynchronous IRQ6 root), the
  // set of addresses that root's own independent walk admitted as retained
  // decoded instructions, with a parallel vector of the owning root id. The
  // cross-root admitted-address supersession filter below consumes all
  // three. This generalizes SEG-007-T144's seed-keyed bookkeeping to root
  // identity; ADR-0013's own `S`/`seeds`/`seed_count` accounting above is
  // completely unaffected by this rename.
  std::vector<StaticProgramRootId> aggregated_secondary_issue_roots;
  std::vector<std::set<Address>> admitted_instruction_addresses_per_root;
  std::vector<StaticProgramRootId> admitted_instruction_addresses_per_root_ids;
  bool aggregation_conflict = false;

  const auto decoded_matches = [](const M68kDecodedInstruction &a, const M68kDecodedInstruction &b) {
    return a.kind == b.kind && a.provenance.source.address.value == b.provenance.source.address.value &&
           a.provenance.source.image_offset.value == b.provenance.source.image_offset.value &&
           a.provenance.length.value == b.provenance.length.value && a.provenance.bytes == b.provenance.bytes;
  };

  // ADR-0021 §2: the single shared per-root merge routine. Applied once per
  // ADR-0013 seed (unchanged ceiling checks stay exactly where they are,
  // above this point) and once for the IRQ6 root -- this is the exact
  // merge body both the former reset-seed loop and the former
  // `handler_walk_clean`-gated IRQ6 block independently duplicated; DRY
  // reuse only, no new architecture. The IRQ6 root's own `primary_issue`/
  // `secondary_issues` flow into the SAME shared `primary_issue`/
  // `aggregated_secondary_issues` aggregation a seed's own issues do,
  // subject to ADR-0021 §4's pinned probe-failure discriminator applied by
  // the caller BEFORE this routine is invoked for the IRQ6 root.
  const auto merge_root_result = [&](const StaticProgramRootId &root_id, const M68kStaticDiscoveryResult &discovery) {
    // ADR-0021 §2a (was SEG-007-T144, seed-keyed): record every address this
    // root's independent walk admitted as a retained decoded instruction.
    std::set<Address> admitted_this_root;
    for (const auto &address : discovery.decode_order)
      if (discovery.decode_cache.find(address) != nullptr) admitted_this_root.insert(address.value);
    admitted_instruction_addresses_per_root.push_back(std::move(admitted_this_root));
    admitted_instruction_addresses_per_root_ids.push_back(root_id);

    for (const auto &address : discovery.decode_order) {
      const auto *decoded = discovery.decode_cache.find(address);
      if (decoded == nullptr) continue;
      const auto found = merged_decoded.find(address.value);
      if (found == merged_decoded.end()) {
        merged_decoded.emplace(address.value, *decoded);
        decode_order.push_back(address.value);
      } else if (!decoded_matches(found->second, *decoded)) {
        aggregation_conflict = true;
        ++analysis.offline_inventory_stitch_metrics.overlapping_unit_conflict_count;
      } else {
        // SEG-007-T180 / ADR-0026: two independently-validated roots agree on
        // this instruction's decode -- deduplicated, never a conflict.
        ++analysis.offline_inventory_stitch_metrics.overlapping_unit_agreement_count;
      }
    }
    for (const auto &address : discovery.block_entries)
      if (block_entries.insert(address.value).second) block_entry_order.push_back(address.value);
    for (const auto &edge : discovery.edges) {
      const auto signature = EdgeSignature{
          edge.source_instruction.source.address.value, static_cast<int>(edge.kind), edge.target.value,
          edge.call.has_value(), edge.call ? edge.call->callee.value : Address{},
          edge.call ? edge.call->continuation.value : Address{}};
      if (edge_signatures_seen.insert(signature).second) merged_edges.push_back(edge);
    }
    for (const auto &frame : discovery.frames) {
      // ADR-0009: an indirect call site's own caller address can legitimately
      // own more than one frame, one per proven candidate callee -- exactly
      // like the edge signature above, only an exact (caller, callee,
      // continuation) duplicate is deduplicated; a differing callee sharing
      // the same caller is a distinct, valid candidate frame, never a
      // conflict.
      const auto key = std::make_tuple(frame.call.caller.source.address.value, frame.call.callee.value,
                                       frame.call.continuation.value);
      if (frame_signatures_seen.insert(key).second) merged_frames.push_back(frame);
    }
    for (const auto &set : discovery.indirect_target_ea_sets) {
      const auto key = set.source_instruction.source.address.value;
      // SEG-007-T174 / ADR-0024 cross-tier exclusivity: one source
      // instruction may hold a proven Tier-1 M68kIndirectTargetEaSet or an
      // unproven Tier-2 M68kUnprovenIndirectControlEaSet, never both --
      // `process_indirect_control` (libs/cpu/m68k/src/static_discovery.cpp) never
      // records both facts for the SAME address within one root's own walk,
      // but `analyze_finite_index_values` is scoped per-root (seeded from
      // that root's own entry and local decode/value-flow history only), so
      // two independent roots can in principle classify the same source
      // address differently: one root's own local walk might prove a finite
      // target set for it while a different root's own local walk -- never
      // having traversed the code establishing the same index register's
      // bound -- records only the weaker unproven fact for it. This was
      // previously unchecked: each map was merged independently with no
      // cross-map lookup, so a genuine cross-root disagreement here would
      // have silently retained both authorities side by side instead of
      // failing closed like every other cross-root disagreement this same
      // routine already guards (decoded-instruction mismatch, indirect
      // candidate-set mismatch, unproven control-EA mismatch, completion-RTS
      // mismatch, below). Fail closed here too, exactly like those siblings,
      // rather than silently choosing one tier over the other -- which tier
      // should win, if either, is a genuine architecture decision this
      // task's own Non-goals forbid making unilaterally.
      // SEG-007-T179 / ADR-0025: Tier-1 strictly supersedes Tier-2 for one
      // source address. A cross-root disagreement where one root proved a
      // finite target set and another only recorded the weaker unproven
      // (An)/pc_index8 fact is resolved in favour of the proven set: drop the
      // unproven fact and keep the proven authority. The proven set is a
      // superset-safe, decode-validated finite enumeration; the unproven fact
      // would only ever have gated Tier-2 emitted-set membership for the same
      // site. This is the deliberate ADR-0025 resolution of the cross-tier
      // ambiguity SEG-007-T174 previously left fail-closed.
      if (unproven_indirect_by_source.find(key) != unproven_indirect_by_source.end()) {
        unproven_indirect_by_source.erase(key);
        unproven_indirect_order.erase(
            std::remove(unproven_indirect_order.begin(), unproven_indirect_order.end(), key),
            unproven_indirect_order.end());
      }
      const auto found = indirect_by_source.find(key);
      if (found == indirect_by_source.end()) {
        indirect_by_source.emplace(key, set);
        indirect_order.push_back(key);
      } else {
        const bool candidates_match = found->second.candidates.size() == set.candidates.size() &&
            std::equal(found->second.candidates.begin(), found->second.candidates.end(), set.candidates.begin(),
                       [](const M68kProgramAddress &a, const M68kProgramAddress &b) { return same(a, b); });
        if (!candidates_match) { aggregation_conflict = true; }
      }
    }
    for (const auto &set : discovery.unproven_indirect_control_ea_sets) {
      const auto key = set.source_instruction.source.address.value;
      // SEG-007-T174 / ADR-0024 cross-tier exclusivity, symmetric case: see
      // the identical check and its own doc comment in the
      // `indirect_target_ea_sets` loop immediately above -- a source address
      // already holding a proven Tier-1 set from an earlier-merged root is
      // the same cross-root disagreement, discovered from the opposite
      // direction (this root's own walk produced only the weaker unproven
      // fact for a source another root already proved finite).
      // SEG-007-T179 / ADR-0025: Tier-1 strictly supersedes Tier-2 (see the
      // symmetric note in the `indirect_target_ea_sets` loop above). A source
      // already holding a proven finite target set keeps it; this root's
      // weaker unproven fact for the same address is dropped rather than
      // failing the whole aggregation closed.
      if (indirect_by_source.find(key) != indirect_by_source.end()) {
        continue;
      }
      const auto found = unproven_indirect_by_source.find(key);
      if (found == unproven_indirect_by_source.end()) {
        unproven_indirect_by_source.emplace(key, set);
        unproven_indirect_order.push_back(key);
      } else if (!same_ea(found->second.control_ea, set.control_ea)) {
        aggregation_conflict = true;
      }
    }
    if (discovery.completion_rts) {
      if (!completion_rts) completion_rts = discovery.completion_rts;
      else if (!same(completion_rts->source.address, discovery.completion_rts->source.address)) {
        aggregation_conflict = true;
      }
    }
    // The first root encountering a stop keeps this profile's established
    // single-primary-issue shape (byte-identical for the round-1-only,
    // single-seed, no-IRQ6-root case); every other root's own primary issue
    // becomes an additional best-effort secondary candidate, exactly like
    // SEG-007-T064's existing sibling exploration secondaries below.
    if (discovery.primary_issue) {
      if (!primary_issue) {
        primary_issue = discovery.primary_issue;
        primary_issue_root = root_id;
      } else {
        aggregated_secondary_issues.push_back(*discovery.primary_issue);
        aggregated_secondary_issue_roots.push_back(root_id);
      }
    }
    for (const auto &issue : discovery.secondary_issues) {
      aggregated_secondary_issues.push_back(issue);
      aggregated_secondary_issue_roots.push_back(root_id);
    }
  };

  // SEG-007-T179 / ADR-0025: the offline whole-ROM code-entry inventory is a
  // set of untrusted, high-recall PROPOSALS. A false-positive candidate that
  // points into data walks up to `m68k_discovery_max_instructions` junk
  // instructions and its own root reports a fatal (non-prefix-boundary)
  // `discovery_budget_exhausted`. Only the reset entry and the IRQ6 autovector
  // root are authoritative for a whole-program rejection; a candidate root's
  // own fatal probe failure EXCLUDES that candidate (it never becomes a
  // reachability root or a retained instruction) rather than failing the
  // entire assisted build closed. The reset entry (`seed_index == 0`) and any
  // runtime_confirmed fallback seed keep the prior authoritative behaviour.
  const auto candidate_seed_values = [&]() {
    std::set<Address> values;
    for (const auto &candidate : program.external_code_entry_candidates) values.insert(candidate.address);
    for (const auto &seed : program.runtime_confirmed_seeds) values.erase(seed.value);
    values.erase(entry.value);
    return values;
  }();
  const auto is_fatal_probe_failure = [&](const M68kDiscoveryIssue &issue) {
    const auto translated = translate_m68k_discovery_issue(program, issue);
    return translated.category == DirectFlowDiagnostic::discovery_budget_exhausted &&
           classify_frontier(translated, false) != GenesisFrontierClass::discovery_prefix_boundary;
  };
  const auto candidate_walk_truncated = [&](const M68kStaticDiscoveryResult &discovery) {
    // Any `discovery_budget_exhausted` on a candidate root -- fatal or
    // prefix-boundary shaped -- means the walk was CUT mid-flow: its trailing
    // block chain is unterminated and unsafe to partially retain. Exclude the
    // whole candidate.
    const auto budget_cut = [&](const M68kDiscoveryIssue &issue) {
      return translate_m68k_discovery_issue(program, issue).category ==
             DirectFlowDiagnostic::discovery_budget_exhausted;
    };
    if (discovery.primary_issue && budget_cut(*discovery.primary_issue)) return true;
    return std::any_of(discovery.secondary_issues.begin(), discovery.secondary_issues.end(), budget_cut);
  };
  // SEG-007-T180 / ADR-0026 Phase 1 (admission): an offline code-entry
  // inventory PARTITIONS validation into bounded, independently-validated
  // local units. Validate each candidate as its own bounded local unit with
  // every OTHER candidate plus the reset entry treated as an independent unit
  // boundary, so this candidate's own local walk never recursively charges
  // another proposed unit's body to its own per-root discovery budget. No
  // Phase-1 result is merged. A candidate is admitted iff its entry decoded
  // cleanly into a retained block, its walk hit no fatal (non-prefix-boundary)
  // probe failure, and its walk was not truncated by a budget cut.
  auto &stitch_metrics = analysis.offline_inventory_stitch_metrics;
  stitch_metrics.offline_candidate_count = static_cast<std::uint32_t>(candidate_seed_values.size());
  // SEG-007-T213 / ADR-0026 §7: Phase-1 admission is a monotonically
  // shrinking, deterministic fixed-point computation, not a single pass. Each
  // round validates every SCHEDULED candidate `c` against a FROZEN snapshot
  // of the round's surviving set, `boundary = (round_members - {c}) +
  // {reset entry}`; every candidate's probe within a round sees the SAME
  // snapshot regardless of another candidate's outcome earlier in that same
  // round (the ADR's literal Jacobi-style formulation, not an in-round
  // Gauss-Seidel mutation). All of a round's removals are collected in
  // `removed_this_round` and applied to `admitted_units` only AFTER every
  // scheduled candidate in that round has been probed, so a same-round
  // cascade is never invisibly absorbed into a single round's own pass.
  // Dependency-aware revalidation bounds each round after the first to only
  // the candidates whose own probe actually consulted a just-removed boundary
  // member as a control-transfer destination
  // (`M68kStaticDiscoveryResult::stitched_boundary_addresses`); a candidate
  // whose walk never reached a removed address is provably unaffected and is
  // not requeued. Final rejection metrics (`rejected_entry_undecodable` /
  // `rejected_fatal_probe_failure` / `rejected_walk_truncated_budget_cut`) are
  // attributed once per candidate, under its LAST (round-of-rejection) probe
  // result, never double-counted across rounds.
  std::set<Address> admitted_units = candidate_seed_values;
  std::set<Address> round0_admitted;
  // Per-candidate dependency set: the OTHER surviving candidate addresses this
  // candidate's own last probe actually stitched at. Rebuilt for every probe.
  std::map<Address, std::set<Address>> dependency_by_candidate;
  // Per-candidate final rejection classification, applied once at the end so
  // an intermediate round's classification is never double-counted.
  enum class AdmissionOutcome { admitted, entry_undecodable, fatal_probe_failure, walk_truncated_budget_cut };
  std::map<Address, AdmissionOutcome> outcome_by_candidate;
  // Per-candidate most-recent local instruction/block counts from its last
  // successful probe, aggregated into `stitch_metrics.aggregate_local_*`
  // exactly once per candidate (on its first successful probe) regardless of
  // how many times dependency-aware revalidation re-probes a candidate that
  // remains admitted across rounds.
  std::set<Address> aggregated_once;
  const auto probe_candidate = [&](Address candidate_value,
                                    const std::set<Address> &round_members) -> AdmissionOutcome {
    std::set<Address> boundary = round_members;
    boundary.erase(candidate_value);
    boundary.insert(entry.value);
    const M68kProgramAddress candidate_address{TargetAddressSpace::m68k_program, candidate_value};
    const auto probe = discover_m68k_static_graph(candidate_address, limits, environment, boundary);
    const bool entry_decoded = probe.decode_cache.find(candidate_address) != nullptr;
    const bool fatal_probe =
        (probe.primary_issue && is_fatal_probe_failure(*probe.primary_issue)) ||
        std::any_of(probe.secondary_issues.begin(), probe.secondary_issues.end(),
                    [&](const M68kDiscoveryIssue &issue) { return is_fatal_probe_failure(issue); });
    const bool truncated = candidate_walk_truncated(probe);
    if (!entry_decoded) return AdmissionOutcome::entry_undecodable;
    if (fatal_probe) return AdmissionOutcome::fatal_probe_failure;
    if (truncated) return AdmissionOutcome::walk_truncated_budget_cut;
    // Record only the OTHER surviving candidates this probe actually stitched
    // at (excludes the reset entry, which is never removed by this
    // computation and therefore never needs revalidation tracking).
    std::set<Address> deps;
    for (const auto stitched : probe.stitched_boundary_addresses)
      if (candidate_seed_values.count(stitched) != 0U) deps.insert(stitched);
    dependency_by_candidate[candidate_value] = std::move(deps);
    const auto local_instr = static_cast<std::uint32_t>(probe.decode_order.size());
    const auto local_blocks = static_cast<std::uint32_t>(probe.block_entries.size());
    stitch_metrics.max_local_discovery_instructions =
        std::max(stitch_metrics.max_local_discovery_instructions, local_instr);
    stitch_metrics.max_local_discovery_blocks =
        std::max(stitch_metrics.max_local_discovery_blocks, local_blocks);
    if (aggregated_once.insert(candidate_value).second) {
      stitch_metrics.aggregate_local_discovery_instructions += local_instr;
      stitch_metrics.aggregate_local_discovery_blocks += local_blocks;
    }
    return AdmissionOutcome::admitted;
  };
  // Round 0: probe every initially-proposed candidate exactly once, against
  // the frozen A0 snapshot.
  std::set<Address> to_probe = candidate_seed_values;
  std::uint32_t rounds = 0U;
  if (!candidate_seed_values.empty()) {
    for (;;) {
      ++rounds;
      // Freeze this round's snapshot BEFORE probing anyone in it; every
      // candidate scheduled this round is validated against this SAME
      // snapshot, never against another candidate's in-round outcome.
      const std::set<Address> round_members = admitted_units;
      std::set<Address> removed_this_round;
      for (const auto candidate_value : to_probe) {
        if (round_members.count(candidate_value) == 0U) continue;  // already rejected in an earlier round
        const auto outcome = probe_candidate(candidate_value, round_members);
        outcome_by_candidate[candidate_value] = outcome;
        if (outcome != AdmissionOutcome::admitted) removed_this_round.insert(candidate_value);
      }
      // Apply this round's removals only now, after every scheduled candidate
      // has been probed against the same frozen snapshot.
      for (const auto removed : removed_this_round) {
        admitted_units.erase(removed);
        dependency_by_candidate.erase(removed);
      }
      if (rounds == 1U) round0_admitted = admitted_units;
      if (removed_this_round.empty()) break;
      // Dependency-aware revalidation: only a surviving candidate whose own
      // recorded dependency set intersects the just-removed set can possibly
      // be affected (ADR-0026 §7's monotonic-rejection proof); it is the only
      // candidate requeued for the next round.
      std::set<Address> requeue;
      for (const auto &[candidate_value, deps] : dependency_by_candidate) {
        for (const auto removed : removed_this_round)
          if (deps.count(removed) != 0U) {
            requeue.insert(candidate_value);
            break;
          }
      }
      if (requeue.empty()) break;
      stitch_metrics.admission_revalidation_count += static_cast<std::uint32_t>(requeue.size());
      to_probe = std::move(requeue);
    }
  }
  stitch_metrics.admission_fixed_point_rounds = candidate_seed_values.empty() ? 0U : rounds;
  for (const auto candidate_value : round0_admitted)
    if (admitted_units.count(candidate_value) == 0U) ++stitch_metrics.rejected_after_dependency_removal_count;
  for (const auto candidate_value : candidate_seed_values) {
    if (admitted_units.count(candidate_value) != 0U) continue;
    const auto it = outcome_by_candidate.find(candidate_value);
    const auto outcome = it != outcome_by_candidate.end() ? it->second : AdmissionOutcome::entry_undecodable;
    switch (outcome) {
      case AdmissionOutcome::entry_undecodable:
        ++stitch_metrics.rejected_entry_undecodable;
        break;
      case AdmissionOutcome::fatal_probe_failure:
        ++stitch_metrics.rejected_fatal_probe_failure;
        break;
      case AdmissionOutcome::walk_truncated_budget_cut:
        ++stitch_metrics.rejected_walk_truncated_budget_cut;
        break;
      case AdmissionOutcome::admitted:
        break;  // unreachable: not in admitted_units implies a rejection outcome was recorded.
    }
  }
  for (const auto &[candidate_value, deps] : dependency_by_candidate)
    stitch_metrics.admission_dependency_edge_count += static_cast<std::uint32_t>(deps.size());
  stitch_metrics.admitted_unit_count = static_cast<std::uint32_t>(admitted_units.size());

  // SEG-007-T181 / ADR-0027 §1/§4: continuation-root FIXPOINT PRE-PASS. Between
  // Phase-1 admission and Phase-2 stitching, discover the set of
  // fallthrough-continuation unit roots: addresses at which a root walk reaches
  // its per-root instruction ceiling on a plain `next_pc` step (sequential
  // fallthrough or branch/call continuation) whose continuation decoded cleanly
  // as a prefix-boundary shape. This mirrors the Phase-1 walk/inspect/discard
  // pattern exactly -- no result is merged, `merge_root_result` and its
  // accumulators are untouched. Each round re-drives every root (entry, admitted
  // units, continuation roots discovered so far, and any runtime-confirmed
  // seeds) with the CURRENT continuation-root set as the SEPARATE 5th-arg
  // fallthrough-continuation boundary set, so a continuation body already owned
  // by another synthesized unit is stitched rather than re-walked. The pass
  // terminates when a round discovers nothing new; monotonic growth bounded by
  // `m68k_fallthrough_continuation_unit_ceiling` guarantees termination. A
  // region that would need more continuation units than the ceiling fails the
  // BUILD closed (no loop, no cap raise).
  // SEG-007-T181 / ADR-0027 §1: the level-6 interrupt autovector handler root
  // (image vector-table offset 0x78) is walked in Phase 2 and is authoritative
  // for a whole-program rejection exactly like the reset entry, so its own
  // per-root ceiling trips must be visible to the continuation pre-pass. Resolve
  // the raw vector here with the same guard the Phase-2 IRQ6 block uses; a
  // zero/absent/odd/unmapped slot simply contributes no pre-pass root.
  const std::optional<Address> irq6_pre_pass_root = [&]() -> std::optional<Address> {
    constexpr std::size_t kIrq6VectorOffset = 0x78U;
    constexpr std::size_t kResetPcOffset = 0x4U;
    constexpr std::size_t kVectorTableBytes = 0x100U;
    const auto &image_bytes = program.image.bytes;
    if (image_bytes.size() < kVectorTableBytes || !program.startup_ingress) return std::nullopt;
    const auto read_be32 = [&](std::size_t off) -> Address {
      return (static_cast<Address>(image_bytes[off]) << 24) |
             (static_cast<Address>(image_bytes[off + 1U]) << 16) |
             (static_cast<Address>(image_bytes[off + 2U]) << 8) |
             static_cast<Address>(image_bytes[off + 3U]);
    };
    if (read_be32(kResetPcOffset) != program.startup_ingress->entry.value) return std::nullopt;
    const Address handler = read_be32(kIrq6VectorOffset);
    if (handler == 0U || (handler & 1U) != 0U) return std::nullopt;
    const M68kProgramAddress handler_address{TargetAddressSpace::m68k_program, handler};
    if (environment.admit_target(handler_address, M68kDiscoveryTargetRole::interrupt_vector)) return std::nullopt;
    return handler;
  }();
  std::set<Address> continuation_roots;
  // SEG-007-T182 / ADR-0028: roots synthesized from statically-resolved+validated
  // direct control-transfer destinations reached at a per-root instruction
  // ceiling. Discovered by the same fixpoint pre-pass as `continuation_roots`
  // but routed through the EXISTING 4th-arg independent_unit_boundaries seam
  // (ADR-0026 §2): the A -> X edge/frame was already recorded by the caller, so
  // only X's body walk is partitioned. Reset/IRQ6-reachable through a validated
  // control edge, so authoritative for a whole-program stop (like continuation
  // roots, unlike a false-positive offline candidate).
  std::set<Address> synthesized_control_roots;
  // ADR-0027 §1: fallthrough-continuation partitioning is an offline-inventory-
  // aware mechanism (it extends ADR-0026). With no offline inventory the route
  // is byte-identical to the pre-T181 walk and no continuation unit is ever
  // synthesized.
  if (!candidate_seed_values.empty()) {
    std::vector<Address> pre_pass_roots;
    pre_pass_roots.push_back(entry.value);
    if (irq6_pre_pass_root) pre_pass_roots.push_back(*irq6_pre_pass_root);
    for (const auto unit : admitted_units) pre_pass_roots.push_back(unit);
    for (const auto &seed : program.runtime_confirmed_seeds)
      if (std::find(pre_pass_roots.begin(), pre_pass_roots.end(), seed.value) == pre_pass_roots.end())
        pre_pass_roots.push_back(seed.value);
    std::uint32_t rounds = 0U;
    for (;;) {
      std::set<Address> discovered;
      std::set<Address> discovered_control;
      std::vector<Address> round_roots = pre_pass_roots;
      for (const auto cr : continuation_roots)
        if (std::find(round_roots.begin(), round_roots.end(), cr) == round_roots.end())
          round_roots.push_back(cr);
      for (const auto sc : synthesized_control_roots)
        if (std::find(round_roots.begin(), round_roots.end(), sc) == round_roots.end())
          round_roots.push_back(sc);
      for (const auto root_value : round_roots) {
        // SEG-007-T182 / ADR-0028: synthesized resolved-control-target roots join
        // the 4th-arg independent-unit boundary set so a body already owned by
        // such a unit is stitched rather than re-charged to this root's ceiling.
        // SEG-007-T213 / ADR-0026 §7 "Reset-entry boundary consistency": every
        // non-reset root's boundary must also include the reset entry, exactly
        // as every Phase-1 admission probe's own boundary already does. For
        // reset's own root walk (`root_value == entry.value`) the `erase`
        // below removes it again, restoring today's existing boundary with no
        // special case.
        std::set<Address> unit_boundary = admitted_units;
        unit_boundary.insert(synthesized_control_roots.begin(), synthesized_control_roots.end());
        unit_boundary.insert(entry.value);
        unit_boundary.erase(root_value);
        std::set<Address> cont_boundary = continuation_roots;
        cont_boundary.erase(root_value);
        const M68kProgramAddress root_address{TargetAddressSpace::m68k_program, root_value};
        const auto probe = discover_m68k_static_graph(root_address, limits, environment,
                                                      unit_boundary, cont_boundary);
        for (const auto &frontier : probe.fallthrough_continuation_frontier)
          if (frontier.space == TargetAddressSpace::m68k_program &&
              continuation_roots.count(frontier.value) == 0U)
            discovered.insert(frontier.value);
        for (const auto &frontier : probe.resolved_control_target_frontier)
          if (frontier.space == TargetAddressSpace::m68k_program &&
              synthesized_control_roots.count(frontier.value) == 0U &&
              continuation_roots.count(frontier.value) == 0U &&
              admitted_units.count(frontier.value) == 0U)
            discovered_control.insert(frontier.value);
      }
      ++rounds;
      if (discovered.empty() && discovered_control.empty()) break;
      // SEG-007-T182 / ADR-0028: one shared COUNT ceiling governs all synthesized
      // partition units (continuation + resolved-control-target). NOT an
      // instruction/block/seed/round budget raise: every synthesized unit is
      // still independently walked under the unchanged per-root
      // m68k_discovery_max_instructions ceiling. A region needing more units than
      // the ceiling fails the build closed (no loop).
      if (static_cast<std::uint64_t>(continuation_roots.size()) + synthesized_control_roots.size() +
              discovered.size() + discovered_control.size() >
          m68k_fallthrough_continuation_unit_ceiling) {
        stitch_metrics.continuation_synthesis_rounds = rounds;
        stitch_metrics.fallthrough_continuation_unit_count =
            static_cast<std::uint32_t>(continuation_roots.size());
        stitch_metrics.synthesized_resolved_control_target_unit_count =
            static_cast<std::uint32_t>(synthesized_control_roots.size());
        auto r = rejected(DirectFlowDiagnostic::discovery_budget_exhausted, program);
        set_source(r, entry);
        return r;
      }
      continuation_roots.insert(discovered.begin(), discovered.end());
      synthesized_control_roots.insert(discovered_control.begin(), discovered_control.end());
    }
    stitch_metrics.continuation_synthesis_rounds = rounds;
    stitch_metrics.fallthrough_continuation_unit_count =
        static_cast<std::uint32_t>(continuation_roots.size());
    stitch_metrics.synthesized_resolved_control_target_unit_count =
        static_cast<std::uint32_t>(synthesized_control_roots.size());
  }

  // SEG-007-T180 / ADR-0026 Phase 2 (stitch + aggregate): walk the reset entry
  // (`seed_index == 0`), each admitted-unit root, and every runtime-confirmed
  // fallback seed, each with `boundary = admitted_units \ {this root's own
  // entry}`. Non-admitted (false-positive) candidates are neither walked nor
  // placed in any boundary set. Every Phase-2 result flows through the
  // unchanged `merge_root_result`.
  seeds.clear();
  seeds.push_back(entry.value);
  for (const auto unit : admitted_units)
    if (std::find(seeds.begin(), seeds.end(), unit) == seeds.end()) seeds.push_back(unit);
  // SEG-007-T181 / ADR-0027 §1: every synthesized fallthrough-continuation root
  // participates in Phase-2 identically to an admitted unit (after the admitted
  // units, before the runtime-confirmed fallback seeds).
  for (const auto cr : continuation_roots)
    if (std::find(seeds.begin(), seeds.end(), cr) == seeds.end()) seeds.push_back(cr);
  // SEG-007-T182 / ADR-0028: synthesized resolved-control-target roots follow,
  // participating in Phase-2 identically to an admitted unit / continuation root.
  for (const auto sc : synthesized_control_roots)
    if (std::find(seeds.begin(), seeds.end(), sc) == seeds.end()) seeds.push_back(sc);
  for (const auto &seed : program.runtime_confirmed_seeds)
    if (std::find(seeds.begin(), seeds.end(), seed.value) == seeds.end()) seeds.push_back(seed.value);
  for (std::size_t seed_index = 0; seed_index < seeds.size(); ++seed_index) {
    const auto seed_value = seeds[seed_index];
    const M68kProgramAddress seed_address{TargetAddressSpace::m68k_program, seed_value};
    // SEG-007-T213 / ADR-0026 §7: the reset entry joins every non-reset
    // seed's boundary; reset's own seed walk (`seed_value == entry.value`)
    // erases it again below, unaffected.
    std::set<Address> boundary = admitted_units;
    boundary.insert(synthesized_control_roots.begin(), synthesized_control_roots.end());
    boundary.insert(entry.value);
    boundary.erase(seed_value);
    std::set<Address> cont_boundary = continuation_roots;
    cont_boundary.erase(seed_value);
    auto discovery =
        discover_m68k_static_graph(seed_address, limits, environment, boundary, cont_boundary);
    stitch_metrics.stitched_direct_edge_count += discovery.stitched_boundary_edges;
    stitch_metrics.stitched_fallthrough_continuation_edge_count +=
        discovery.stitched_fallthrough_continuation_edges;
    // SEG-007-T181 / ADR-0027 §5/§6: a synthesized fallthrough-continuation
    // root represents reset/IRQ6-reachable linear code, so -- unlike a
    // false-positive inventory candidate -- its own walk IS authoritative for a
    // whole-program stop. Its frontier issues are NOT dropped here; a genuine
    // unresolved open control edge or decode failure inside a continuation unit
    // still fails the build closed exactly as the reset entry would.
    if (seed_index != 0 && candidate_seed_values.count(seed_value) != 0U &&
        continuation_roots.count(seed_value) == 0U &&
        synthesized_control_roots.count(seed_value) == 0U) {
      // SEG-007-T180 / ADR-0026: an admitted-unit root is not authoritative
      // for a whole-program rejection -- only the reset entry
      // (`seed_index == 0`) and the IRQ6 autovector root are. Drop this unit
      // root's own frontier issues; still merge every instruction / edge /
      // block it decoded (the unchanged `decoded_matches` and per-root
      // supersession guards still fail closed on a genuine cross-root decode
      // conflict). Phase 1 already classified every candidate's admission and
      // populated the normalized rejection metrics; a non-admitted candidate
      // is never a Phase-2 seed at all.
      //
      // SEG-007-T184 / ADR-0009 / ADR-0024: EXCEPT an issue whose own address
      // already carries a Tier-1 `M68kIndirectTargetEaSet` or Tier-2
      // `M68kUnprovenIndirectControlEaSet` fact from this SAME root's own
      // walk. Such an address is never "walked into junk data" the way an
      // ordinary false-positive candidate is -- it is a genuine, precisely
      // classified computed control-transfer terminal whose fact is merged
      // unconditionally regardless of this drop (`merge_root_result` never
      // filters either fact vector). `compute_candidate_frontier_addresses`
      // (below) folds every such fact's own address into the shared allowed-
      // edge-target set, which unconditionally excludes it from ordinary
      // static-block terminal-hood (`build_analysis`'s `exclude_frontier_
      // instruction`); dropping its own issue here as well would leave it
      // excluded from blocks yet never independently represented as a
      // frontier, tripping `partial_or_rejection`'s own sibling-address
      // representation invariant and rejecting the whole build over a site
      // Tier-2 is specifically designed to handle. Preserving it as a
      // (never-primary, never-authoritative) secondary lets it flow through
      // the ordinary classify/eligibility/known_but_unemitted_target path
      // every other secondary already uses -- it still cannot fail the whole
      // build (only `primary_issue` can), and every genuine false-positive
      // candidate's own unrelated issues are dropped exactly as before.
      const auto carries_indirect_fact = [&](const M68kDiscoveryIssue &issue) {
        return std::any_of(discovery.indirect_target_ea_sets.begin(), discovery.indirect_target_ea_sets.end(),
                            [&](const M68kIndirectTargetEaSet &set) {
                              return set.source_instruction.source.address.value == issue.address.value;
                            }) ||
               std::any_of(discovery.unproven_indirect_control_ea_sets.begin(),
                           discovery.unproven_indirect_control_ea_sets.end(),
                           [&](const M68kUnprovenIndirectControlEaSet &set) {
                             return set.source_instruction.source.address.value == issue.address.value;
                           });
      };
      std::vector<M68kDiscoveryIssue> preserved_secondary;
      if (discovery.primary_issue && carries_indirect_fact(*discovery.primary_issue))
        preserved_secondary.push_back(*discovery.primary_issue);
      for (const auto &secondary_issue : discovery.secondary_issues)
        if (carries_indirect_fact(secondary_issue)) preserved_secondary.push_back(secondary_issue);
      discovery.primary_issue.reset();
      discovery.secondary_issues = std::move(preserved_secondary);
    }
    merge_root_result({StaticProgramRootKind::synchronous_entry, seed_index}, discovery);
  }

  // ADR-0021 (was ADR-0020 §6, now generalized): the MC68000 level-6
  // interrupt autovector as a bounded static hardware discovery root. It is
  // resolved from the mapped cartridge image's fixed vector-table offset
  // 0x78 exactly as the reset PC at 0x4 is, then walked by ONE extra
  // discover_m68k_static_graph pass whose facts are folded into the same
  // cross-root merge structures under the same dedup rules via the shared
  // `merge_root_result` routine above. This root NEVER enters seed set S
  // (`seeds`) and never increments `seed_index`/seed_count -- ADR-0013 §7's
  // `S = {reset entry} ∪ runtime_confirmed_seeds` is preserved verbatim.
  // (Per ADR-0013 §7c, `S` itself carries no total-cardinality ceiling here
  // any more; see the `seeds` construction comment above.) An
  // odd/unmapped/unrepresentable vector fails the BUILD closed.
  const auto resolve_vector_handler = [&](std::size_t vector_offset) -> std::optional<Address> {
    constexpr std::size_t kResetPcOffset = 0x4U;
    constexpr std::size_t kVectorTableBytes = 0x100U;
    const auto &image_bytes = program.image.bytes;
    if (image_bytes.size() < kVectorTableBytes || !program.startup_ingress) return std::nullopt;
    const auto read_be32 = [&](std::size_t off) -> Address {
      return (static_cast<Address>(image_bytes[off]) << 24) |
             (static_cast<Address>(image_bytes[off + 1U]) << 16) |
             (static_cast<Address>(image_bytes[off + 2U]) << 8) |
             static_cast<Address>(image_bytes[off + 3U]);
    };
    if (read_be32(kResetPcOffset) != program.startup_ingress->entry.value) return std::nullopt;
    const Address handler = read_be32(vector_offset);
    return handler == 0U ? std::nullopt : std::optional<Address>{handler};
  };

  std::optional<Address> irq6_handler_entry_value;
  {
    constexpr std::size_t kIrq6VectorOffset = 0x78U;
    // The autovector root is resolved from the mapped cartridge image's fixed
    // MC68000 exception vector table exactly as the reset PC at offset 0x4 is.
    // Guard: only when the image actually carries a full vector table AND its
    // reset-PC slot matches the resolved startup entry (proving offsets 0-0xFF
    // really are that table, so 0x78 is too). Otherwise -- a synthetic
    // code-only image, or a zeroed (never-installed) slot -- this program
    // establishes no IRQ6 handler and the mechanism stays inert, not a build
    // failure. A non-zero slot resolving to an odd/unmapped/24-bit-invalid
    // address fails the BUILD closed (direct_call precedent).
    const auto resolved_handler = resolve_vector_handler(kIrq6VectorOffset);
    if (!resolved_handler) {
      // inert: no IRQ6 handler installed by this program.
    } else {
    const Address handler = *resolved_handler;
    const M68kProgramAddress handler_address{TargetAddressSpace::m68k_program, handler};
    if (const auto issue =
            environment.admit_target(handler_address, M68kDiscoveryTargetRole::interrupt_vector)) {
      auto r = rejected(issue->category, program);
      set_source(r, entry);
      return r;
    }
    // SEG-007-T180 / ADR-0026: the IRQ6 autovector root also stitches at
    // admitted-unit boundaries rather than recursively rediscovering another
    // proposed unit's body inside its own per-root discovery budget.
    // SEG-007-T213 / ADR-0026 §7: the reset entry joins this root's boundary
    // like every other non-reset root; if the handler address ever equalled
    // the reset entry the `erase(handler)` below would remove it again.
    std::set<Address> irq6_boundary = admitted_units;
    irq6_boundary.insert(synthesized_control_roots.begin(), synthesized_control_roots.end());
    irq6_boundary.insert(entry.value);
    irq6_boundary.erase(handler);
    auto discovery = discover_m68k_static_graph(handler_address, limits, environment, irq6_boundary,
                                               continuation_roots);
    stitch_metrics.stitched_direct_edge_count += discovery.stitched_boundary_edges;
    stitch_metrics.stitched_fallthrough_continuation_edge_count +=
        discovery.stitched_fallthrough_continuation_edges;
    // ADR-0021 §4: the exact pinned fatal-probe-failure discriminator,
    // evaluated directly against the IRQ6 root's own raw discovery result --
    // its own `primary_issue` (if present), then each of its own
    // `secondary_issues`, in that order (first match wins) -- BEFORE any
    // part of this result is folded into the shared aggregation, so it can
    // never be silently orphaned as a dropped provenance-less secondary
    // regardless of processing order (this check fires whether or not a
    // synchronous seed has already occupied the aggregate `primary_issue`).
    // An issue is a fatal probe failure iff its translated category is
    // discovery_budget_exhausted AND classify_frontier for it is NOT
    // discovery_prefix_boundary. On a match, reject the WHOLE candidate
    // build immediately using that translated issue, mirroring an ordinary
    // whole-program rejection shape; do not merge any part of this root's
    // result in that case.
    // (Shared with the synchronous seed loop above -- see its ADR-0025 note.)
    if (discovery.primary_issue && is_fatal_probe_failure(*discovery.primary_issue))
      return translate_m68k_discovery_issue(program, *discovery.primary_issue);
    for (const auto &issue : discovery.secondary_issues) {
      if (is_fatal_probe_failure(issue)) return translate_m68k_discovery_issue(program, issue);
    }
    // ADR-0021 §2: no probe-failure discriminator matched -- the vector
    // validly resolves, so set the handler entry unconditionally and merge
    // this root's result exactly like a seed's. Dispatchability is then
    // already automatically and correctly gated purely by whether the
    // handler's entry block ends up actually retained/emitted
    // (`blocks.contains(...)` at emission time in libs/codegen/c11/src/
    // frontend.cpp), requiring no new gating logic here.
    irq6_handler_entry_value = handler;
    merge_root_result({StaticProgramRootKind::asynchronous_hardware, 0U}, discovery);
    }
  }

  // ADR-0037: vector 5 is a distinct synchronous-exception root. It shares
  // build-time vector resolution, bounded discovery, and aggregation with the
  // IRQ6 root, while remaining outside asynchronous IRQ scheduling semantics.
  std::optional<Address> divide_by_zero_handler_entry_value;
  {
    constexpr std::size_t kDivideByZeroVectorOffset = 0x14U;
    if (const auto resolved_handler = resolve_vector_handler(kDivideByZeroVectorOffset)) {
      const Address handler = *resolved_handler;
      const M68kProgramAddress handler_address{TargetAddressSpace::m68k_program, handler};
      if (const auto issue = environment.admit_target(
              handler_address, M68kDiscoveryTargetRole::synchronous_exception_vector)) {
        auto r = rejected(issue->category, program);
        set_source(r, entry);
        return r;
      }
      std::set<Address> exception_boundary = admitted_units;
      exception_boundary.insert(synthesized_control_roots.begin(), synthesized_control_roots.end());
      exception_boundary.insert(entry.value);
      exception_boundary.erase(handler);
      auto discovery = discover_m68k_static_graph(handler_address, limits, environment, exception_boundary,
                                                   continuation_roots);
      stitch_metrics.stitched_direct_edge_count += discovery.stitched_boundary_edges;
      stitch_metrics.stitched_fallthrough_continuation_edge_count +=
          discovery.stitched_fallthrough_continuation_edges;
      if (discovery.primary_issue && is_fatal_probe_failure(*discovery.primary_issue))
        return translate_m68k_discovery_issue(program, *discovery.primary_issue);
      for (const auto &issue : discovery.secondary_issues)
        if (is_fatal_probe_failure(issue)) return translate_m68k_discovery_issue(program, issue);
      divide_by_zero_handler_entry_value = handler;
      merge_root_result({StaticProgramRootKind::synchronous_exception, 0U}, discovery);
    }
  }

  // SEG-007-T181 / ADR-0027 §6: count reset/IRQ6 prefix-boundary-shaped
  // discovery_budget_exhausted issues whose address is NOT a synthesized
  // continuation root. These are the genuine open-control-edge frontier
  // diagnostics (an unproven indirect site, an undecodable taken target, an
  // unmapped destination) -- unchanged downstream, never eligible for
  // continuation synthesis.
  {
    const auto is_uneligible_ceiling_trip = [&](const M68kDiscoveryIssue &issue) {
      const auto translated = translate_m68k_discovery_issue(program, issue);
      return translated.category == DirectFlowDiagnostic::discovery_budget_exhausted &&
             classify_frontier(translated, false) ==
                 GenesisFrontierClass::discovery_prefix_boundary &&
             continuation_roots.count(issue.address.value) == 0U &&
             synthesized_control_roots.count(issue.address.value) == 0U;
    };
    std::uint32_t uneligible = 0U;
    if (primary_issue && is_uneligible_ceiling_trip(*primary_issue)) ++uneligible;
    for (const auto &issue : aggregated_secondary_issues)
      if (is_uneligible_ceiling_trip(issue)) ++uneligible;
    stitch_metrics.ceiling_trips_not_continuation_eligible = uneligible;
  }

  if (stitch_metrics.offline_candidate_count != 0U)
    std::fprintf(stderr,
                 "segarecomp: offline inventory stitch: candidates=%u admitted=%u "
                 "rejected_fatal_probe=%u rejected_walk_truncated=%u rejected_entry_undecodable=%u "
                 "rejected_other=%u stitched_edges=%u overlap_agree=%u overlap_conflict=%u "
                 "max_local_instr=%u max_local_blocks=%u agg_local_instr=%u agg_local_blocks=%u "
                 "stitched_fallthrough_continuation_edges=%u fallthrough_continuation_units=%u "
                 "continuation_synthesis_rounds=%u ceiling_trips_not_continuation_eligible=%u "
                 "synthesized_resolved_control_target_units=%u "
                 "admission_fixed_point_rounds=%u admission_revalidation_count=%u "
                 "admission_dependency_edge_count=%u rejected_after_dependency_removal_count=%u\n",
                 stitch_metrics.offline_candidate_count, stitch_metrics.admitted_unit_count,
                 stitch_metrics.rejected_fatal_probe_failure,
                 stitch_metrics.rejected_walk_truncated_budget_cut,
                 stitch_metrics.rejected_entry_undecodable, stitch_metrics.rejected_other,
                 stitch_metrics.stitched_direct_edge_count,
                 stitch_metrics.overlapping_unit_agreement_count,
                 stitch_metrics.overlapping_unit_conflict_count,
                 stitch_metrics.max_local_discovery_instructions,
                 stitch_metrics.max_local_discovery_blocks,
                 stitch_metrics.aggregate_local_discovery_instructions,
                 stitch_metrics.aggregate_local_discovery_blocks,
                 stitch_metrics.stitched_fallthrough_continuation_edge_count,
                 stitch_metrics.fallthrough_continuation_unit_count,
                 stitch_metrics.continuation_synthesis_rounds,
                 stitch_metrics.ceiling_trips_not_continuation_eligible,
                 stitch_metrics.synthesized_resolved_control_target_unit_count,
                 stitch_metrics.admission_fixed_point_rounds, stitch_metrics.admission_revalidation_count,
                 stitch_metrics.admission_dependency_edge_count,
                 stitch_metrics.rejected_after_dependency_removal_count);

  if (aggregation_conflict) {
    auto r = rejected(DirectFlowDiagnostic::startup_graph_mismatch, program);
    set_source(r, entry);
    return r;
  }

  // ADR-0021 §2a (was SEG-007-T144's seed-keyed supersession, generalized to
  // StaticProgramRootId): deterministic cross-root admitted-address
  // supersession (ADR-0013 Decision §7 prefix expansion past a
  // runtime_confirmed boundary; ADR-0011 §4.1 independent per-root walks). A
  // per-root unresolved candidate frontier means only that *that walk* did
  // not admit the target. When a DIFFERENT, independently valid root walk
  // (a synchronous seed or the asynchronous IRQ6 root, in either direction)
  // subsequently admits that exact address as a retained decoded
  // instruction -- with no `aggregation_conflict` raised, i.e. every
  // genuinely contradictory fact (decoded_matches mismatch, indirect
  // target-EA candidate-set mismatch, completion-RTS mismatch) has already
  // failed the whole translation closed just above -- the aggregate holds
  // strictly stronger evidence: that address is retained code and can no
  // longer also be represented as an unresolved sibling candidate. Drop any
  // aggregated secondary issue whose own source address was admitted as a
  // retained instruction by some OTHER root's walk, keeping the parallel
  // root-id vector in sync. This supersedes the previous reliance on
  // `partial_or_rejection`'s `retained_addresses != sibling_addresses` check
  // to fail the whole promotion closed for exactly this shape; that check
  // remains unmodified for every genuine "candidate that cannot be
  // represented" case. With a single root (`s == issue_root` for the only
  // root), the `s != issue_root` guard is never satisfiable, so nothing is
  // filtered and single-root discovery stays byte-identical.
  //
  // SEG-007-T213 correction (primary-vs-secondary cross-root admitted-address
  // supersession asymmetry): `merge_root_result` stores the FIRST unresolved
  // issue reported by any root as the aggregate `primary_issue`, separately
  // from every subsequent issue, which lands in `aggregated_secondary_issues`.
  // The supersession rule below was previously applied only to the secondary
  // vector, so an address that happened to be the very first root's own
  // unresolved issue could survive as `primary_issue` even though a
  // different, independently walked root admits that exact address as
  // retained decoded code with consistent facts -- exactly the shape this
  // mechanism exists to supersede. `primary_issue` and
  // `aggregated_secondary_issues` are equivalent representations of "this
  // root's own unresolved issue"; the split is bookkeeping order, not a
  // semantic distinction, so the identical predicate must apply to both.
  // Factor the predicate once (`issue_is_superseded`) and apply it to
  // `primary_issue` too, below; if it supersedes the primary, deterministically
  // promote the next surviving secondary issue (if any) to primary so a
  // genuine unresolved issue is never silently dropped from `failure`
  // reporting.
  const auto issue_is_superseded = [&](const M68kDiscoveryIssue &issue, StaticProgramRootId issue_root) -> bool {
    const auto issue_address = issue.address.value;
    bool superseded = false;
    for (std::size_t s = 0; s < admitted_instruction_addresses_per_root.size(); ++s) {
      if (admitted_instruction_addresses_per_root_ids[s] == issue_root) continue;
      if (admitted_instruction_addresses_per_root[s].count(issue_address) != 0) {
        superseded = true;
        break;
      }
    }
    // SEG-007-T174 / ADR-0024 correction (reviewer-directed, composed-hints
    // regression): decode retention alone is NOT strong enough evidence
    // that "a different root genuinely resolved/continued past" a
    // computed indirect control-transfer instruction (JMP/JSR with a
    // `pc_index8` control EA). Unlike an ordinary straight-line/direct-
    // control instruction, decode retention for one of these is
    // unconditional even when `process_indirect_control` FAILS for every
    // root that independently reaches it (see that function's own doc
    // comment: "the instruction's raw decode is still unconditionally
    // retained and merged ... before the per-kind handler runs") -- so two
    // independently-walked roots can each retain the identical address
    // while each independently and identically failing to resolve it (one
    // via an unprovable index value -> a Tier-2 fact; another via a
    // provably-finite-but-wholly-inadmissible candidate set -> no fact at
    // all), with NEITHER root ever proving a genuine Tier-1 target set for
    // it. This supersession's own stated rationale ("that address is
    // retained code and can no longer also be represented as an unresolved
    // sibling candidate") is correct for an ordinary instruction, where
    // retained decode does imply a different root's walk genuinely
    // continued past it -- but is provably false for a computed indirect
    // site with no Tier-1 fact anywhere in `indirect_by_source`: only a
    // genuine, proven Tier-1 `M68kIndirectTargetEaSet` is strong enough
    // evidence that address is resolved code. Un-supersede exactly this
    // narrow shape (never any other instruction kind, never a genuinely
    // Tier-1-proven indirect site) so it remains a legitimate open
    // candidate frontier instead of silently becoming part of an emitted
    // block whose own terminal target set was never actually proven by
    // anyone -- the exact defect a real, substantially wider Ghidra-
    // assisted candidate-root set first exposed.
    if (superseded) {
      const auto decoded_here = merged_decoded.find(issue_address);
      const bool is_unproven_computed_indirect_site =
          decoded_here != merged_decoded.end() &&
          (decoded_here->second.kind == M68kInstructionKind::jmp ||
           decoded_here->second.kind == M68kInstructionKind::jsr) &&
          m68k_is_supported_computed_control_ea(decoded_here->second.source_ea) &&
          indirect_by_source.find(issue_address) == indirect_by_source.end();
      if (is_unproven_computed_indirect_site) return false;
    }
    return superseded;
  };
  {
    std::vector<M68kDiscoveryIssue> kept_issues;
    std::vector<StaticProgramRootId> kept_roots;
    kept_issues.reserve(aggregated_secondary_issues.size());
    kept_roots.reserve(aggregated_secondary_issue_roots.size());
    for (std::size_t i = 0; i < aggregated_secondary_issues.size(); ++i) {
      const auto issue_root = aggregated_secondary_issue_roots[i];
      if (issue_is_superseded(aggregated_secondary_issues[i], issue_root)) continue;
      kept_issues.push_back(std::move(aggregated_secondary_issues[i]));
      kept_roots.push_back(issue_root);
    }
    aggregated_secondary_issues = std::move(kept_issues);
    aggregated_secondary_issue_roots = std::move(kept_roots);
  }

  // SEG-007-T213: apply the identical supersession predicate to the
  // aggregate `primary_issue`. If the primary issue is superseded, promote
  // the first surviving secondary issue (if any) to primary, keeping both
  // vectors and the promoted issue's own root in sync; otherwise leave no
  // primary issue.
  //
  // A promoted replacement must itself be eligible to SERVE as the aggregate
  // primary: `partial_or_rejection` below classifies the primary issue with
  // `classify_frontier` and, unlike every secondary issue, applies NO
  // `known_but_unemitted_target` fallback for it -- a primary issue whose
  // category `classify_frontier` does not recognize fails the WHOLE build
  // closed (`reject()`). Promoting an arbitrary secondary (e.g. an
  // `odd_direct_target` issue, which only ever qualifies under a secondary's
  // own weaker fallback) would therefore risk turning an existing partial
  // success into a new whole-build rejection -- strictly worse than leaving
  // the stale (superseded) primary issue's address representation in place,
  // which this task's own Non-goals do not intend. Only promote the first
  // surviving secondary that itself already passes `classify_frontier`
  // (mirroring the exact test `partial_or_rejection` performs); if none
  // qualifies, leave `primary_issue` unsuperseded rather than risk that
  // regression -- a narrower, safe residual this task's Case A explicitly
  // permits reporting rather than resolving.
  if (primary_issue && primary_issue_root &&
      issue_is_superseded(*primary_issue, *primary_issue_root)) {
    if (aggregated_secondary_issues.empty()) {
      primary_issue.reset();
      primary_issue_root.reset();
    } else {
      std::optional<std::size_t> promote_index;
      for (std::size_t i = 0; i < aggregated_secondary_issues.size(); ++i) {
        const auto translated = translate_m68k_discovery_issue(program, aggregated_secondary_issues[i]);
        if (classify_frontier(translated, aggregated_secondary_issues[i].access.has_value())) {
          promote_index = i;
          break;
        }
      }
      if (promote_index) {
        primary_issue = std::move(aggregated_secondary_issues[*promote_index]);
        primary_issue_root = aggregated_secondary_issue_roots[*promote_index];
        aggregated_secondary_issues.erase(aggregated_secondary_issues.begin() +
                                           static_cast<std::ptrdiff_t>(*promote_index));
        aggregated_secondary_issue_roots.erase(aggregated_secondary_issue_roots.begin() +
                                                static_cast<std::ptrdiff_t>(*promote_index));
      }
    }
  }

  // SEG-007-T151 (ADR-0013 Decision §7 Phase B / ADR-0011 §4.1): a per-seed
  // walk's own `return_to_continuation` synthesis (`StaticGraphWalk::
  // synthesize_return_edges`) only traces reachable exit RTS instructions
  // through that SAME walk's own local decode cache. When the seed that
  // discovers a call's own frame does not itself decode far enough into the
  // callee body (e.g. past an internal branch its own bounded budget never
  // reached) to reach one of the callee's legitimate exit RTS instructions,
  // no per-seed walk alone ever emits that RTS's own return edge -- even
  // though a DIFFERENT, independently promoted seed's own walk may
  // separately admit that exact RTS as a retained decoded instruction (this
  // is exactly the admitted-but-edgeless shape the aggregate now has
  // strictly stronger evidence for, mirroring the admitted-address
  // supersession above). Re-running the identical, already-correct
  // reachable-RTS trace (`m68k_reachable_return_edges`,
  // static_program.cpp/hpp -- the exact function every per-seed walk itself
  // uses) over the merged cross-seed decode map and merged call-frame set
  // recovers exactly those missing edges, purely additively: every edge it
  // returns is deterministically re-derived from already-verified facts (a
  // retained call frame, plus a decoded successor chain from that frame's
  // own callee entry to the RTS, both already established above with no
  // aggregation_conflict), so folding it through the same EdgeSignature
  // dedup as every per-seed edge either reproduces an edge some seed already
  // contributed (a no-op) or adds a genuinely new completion edge -- never a
  // narrower/broader edge-identity or conflict rule. With a single seed this
  // reproduces exactly that seed's own `synthesize_return_edges` output, so
  // single-seed discovery stays byte-identical.
  // SEG-007-T190 / ADR-0031: baseline Tier-1-UNAWARE cross-seed reachable-RTS
  // completion (unchanged). The bounded fixed point below re-runs this exact
  // trace Tier-1-AWARE once a late computed-JMP target has been represented
  // and accepted, so an RTS reached only through a proven computed jump still
  // receives its `return_to_continuation` edge under the original enclosing
  // call identity.
  for (const auto &edge : m68k_reachable_return_edges(merged_decoded, merged_frames)) {
    const auto signature = EdgeSignature{
        edge.source_instruction.source.address.value, static_cast<int>(edge.kind), edge.target.value,
        edge.call.has_value(), edge.call ? edge.call->callee.value : Address{},
        edge.call ? edge.call->continuation.value : Address{}};
    if (edge_signatures_seen.insert(signature).second) merged_edges.push_back(edge);
  }

  // ADR-0009's ordinary walker intentionally computes finite-register state
  // within one bounded unit.  ADR-0026 subsequently stitches independently
  // validated units into one graph, so a finite producer before such a boundary
  // could not reach a computed JMP in the following unit.  Diagnose and repair
  // only that already-represented relation here: the CPU helper reuses the
  // exact lattice, transfer, merge cap, A7 exclusion, and call-footprint rules
  // over the final decode graph.
  //
  // SEG-007-T190 / ADR-0031: a late computed-JMP target whose ONLY static path
  // is the exact JMP the proof itself resolves is not yet in
  // `merged_decoded`/`block_entries`, so the existing unweakened acceptance
  // gate below silently rejects it before Tier-1 acceptance is even reached.
  // Close this by REPRESENTATION, not relaxation: feed exactly those
  // sound-but-unrepresented candidates back through the EXISTING
  // discover_m68k_static_graph admission probe + `merge_root_result` merge
  // (Stage A), then re-run the same unweakened gate (now satisfiable), then
  // re-run the reachable-RTS trace Tier-1-aware (Stage B). Because a newly
  // represented candidate's own body can contain its own computed-control site
  // that only a subsequent proof pass can resolve, this is a genuinely
  // iterable relation: run it as a bounded monotone fixed point over
  // {fresh proof -> committed-relation revalidation -> Stage A representation
  // -> (only on a graph-stable round) acceptance -> Stage B -> retention},
  // terminating when a round grows no representation, commits no new Tier-1
  // target, and adds no new edge.
  //
  // SEG-007-T190 pre-merge soundness correction: representation is monotone but
  // a finite-An Tier-1 fact is NOT monotone under graph expansion. A relation
  // is therefore only COMMITTED on a round where the graph was stable across
  // both its proof and its acceptance gate, and every committed relation is
  // re-proven and revalidated against every later expanded graph; a committed
  // relation that disappears, widens to unknown, or changes incompatibly fails
  // the build closed rather than silently retaining a stale target set.
  {
    std::vector<M68kProgramAddress> analysis_roots_base{
        {TargetAddressSpace::m68k_program, entry.value}};
    if (irq6_handler_entry_value)
      analysis_roots_base.push_back({TargetAddressSpace::m68k_program, *irq6_handler_entry_value});
    if (divide_by_zero_handler_entry_value)
      analysis_roots_base.push_back({TargetAddressSpace::m68k_program, *divide_by_zero_handler_entry_value});
    // Runtime-confirmed roots are authoritative unknown-state roots, never
    // register facts.
    for (const auto &root : program.runtime_confirmed_seeds)
      if (root.space == TargetAddressSpace::m68k_program) analysis_roots_base.push_back(root);

    // ADR-0031 iteration ceiling. Each productive round strictly grows the
    // merged decode/block-entry representation by at least one previously
    // unrepresented late Tier-1 target address and/or the monotonically
    // growing accepted-Tier-1 source set; both are monotone (never shrink)
    // over a finite address space, so the closure is a monotone fixed point
    // and always terminates. `8` is deliberately chosen (not copied from an
    // unrelated ceiling): it is generous headroom over the deepest
    // computed-dispatch-through-computed-dispatch nesting a Genesis startup
    // route realistically exhibits -- single-level on the T189 Sonic
    // frontier, and classic jump-table dispatch rarely chains past 2-3
    // levels -- while keeping the added work bounded and cheap. It is
    // independent of, and far smaller than, `m68k_fallthrough_continuation_
    // unit_ceiling` (which bounds a different quantity: total synthesized
    // partition units). A route that genuinely needs more rounds fails the
    // BUILD closed (below), never loops and never drops a sound candidate.
    constexpr std::uint32_t kM68kLateIndirectClosureRoundCeiling = 8U;
    std::map<Address, M68kIndirectTargetEaSet> accepted_tier1_by_source;
    std::set<Address> resolved_sources_all;
    std::set<Address> late_represented_targets;
    std::uint32_t synthetic_root_counter = 1U;
    std::uint32_t closure_rounds = 0U;
    bool converged = false;
    for (;;) {
      bool round_changed = false;
      std::vector<M68kProgramAddress> analysis_roots = analysis_roots_base;
      std::vector<M68kProgramAddress> disconnected_root_candidates;
      for (const auto root : admitted_units)
        disconnected_root_candidates.push_back({TargetAddressSpace::m68k_program, root});
      for (const auto root : continuation_roots)
        disconnected_root_candidates.push_back({TargetAddressSpace::m68k_program, root});
      for (const auto root : synthesized_control_roots)
        disconnected_root_candidates.push_back({TargetAddressSpace::m68k_program, root});
      // Late-represented targets are ordinary disconnected-component root
      // candidates for the next proof pass, exactly like an admitted unit.
      for (const auto root : late_represented_targets)
        disconnected_root_candidates.push_back({TargetAddressSpace::m68k_program, root});
      // PROVISIONAL proof over the CURRENT complete graph. Its finite-An facts
      // are only provisional: representation growth this same round (Stage A
      // below) or in any later round can widen a register state toward unknown
      // or change a finite candidate set. A relation is only ever COMMITTED to
      // Tier-1 authority on a round where the graph was stable across both this
      // proof and its acceptance gate (Stage A added no new representation),
      // and every committed relation is revalidated against every later
      // expanded graph before it keeps authorizing an edge / Stage-B traversal.
      const auto provisional_sets =
          m68k_prove_stitched_an_indirect_targets(merged_decoded, analysis_roots, merged_edges,
                                                   limits.max_call_frame_depth, environment,
                                                   disconnected_root_candidates);
      // (source -> sorted finite candidate value set) the provisional proof
      // yields over the current graph, or nullopt when the proof no longer
      // proves any finite set for that source (disappeared / widened to
      // unknown).
      const auto provisional_candidates_for =
          [&](Address source) -> std::optional<std::set<Address>> {
        for (const auto &set : provisional_sets) {
          if (set.source_instruction.source.address.value != source) continue;
          std::set<Address> values;
          for (const auto &candidate : set.candidates) values.insert(candidate.value);
          return values;
        }
        return std::nullopt;
      };

      // --- Revalidation of every closure-owned COMMITTED Tier-1 relation
      // against the current (possibly later-expanded) graph. Representation is
      // monotone; a finite-An fact is NOT monotone under graph expansion -- a
      // newly represented authoritative reaching path can widen the register
      // state toward unknown or change the finite set. A committed relation
      // that disappears, widens to unknown, or changes incompatibly fails the
      // build closed (ADR-0028 fail-closed pattern); it is never silently
      // retained with a stale target set.
      for (const auto &[committed_source, committed_set] : accepted_tier1_by_source) {
        std::set<Address> committed_values;
        for (const auto &candidate : committed_set.candidates) committed_values.insert(candidate.value);
        const auto current = provisional_candidates_for(committed_source);
        if (!current.has_value() || *current != committed_values) {
          stitch_metrics.late_indirect_closure_rounds = closure_rounds;
          stitch_metrics.late_indirect_closure_converged = false;
          auto r = rejected(DirectFlowDiagnostic::startup_graph_mismatch, program);
          set_source(r, entry);
          return r;
        }
      }

      // --- Stage A (representation). Sound candidate addresses only ever
      // come from the Tier-1 proof itself; a candidate that fails the
      // existing admission criteria (undecodable entry, fatal probe failure,
      // budget truncation) is not represented, exactly like any other
      // rejected candidate today. Representation growth is tracked separately
      // from `round_changed`: a round that grew representation must NOT commit
      // (its provisional proof predates the expansion).
      bool representation_changed = false;
      std::set<Address> to_represent;
      for (const auto &set : provisional_sets) {
        const auto source = set.source_instruction.source.address.value;
        const auto decoded = merged_decoded.find(source);
        // Only an existing unproven JMP relation -- JSR needs candidate-
        // specific frames not created by this pass and stays fail-closed.
        if (decoded == merged_decoded.end() || decoded->second.kind != M68kInstructionKind::jmp ||
            indirect_by_source.contains(source) || !unproven_indirect_by_source.contains(source))
          continue;
        for (const auto &candidate : set.candidates) {
          if (candidate.space != TargetAddressSpace::m68k_program) continue;
          if (environment.admit_target(candidate, M68kDiscoveryTargetRole::direct_call)) continue;
          if (merged_decoded.contains(candidate.value) && block_entries.contains(candidate.value)) continue;
          to_represent.insert(candidate.value);
        }
      }
      for (const auto candidate_value : to_represent) {  // std::set: ascending, deterministic
        const M68kProgramAddress late_target_address{TargetAddressSpace::m68k_program, candidate_value};
        // SEG-007-T213 / ADR-0026 §7: the reset entry joins this non-reset
        // root's boundary like every other post-admission walk that reuses
        // this seam.
        std::set<Address> boundary = admitted_units;
        boundary.insert(synthesized_control_roots.begin(), synthesized_control_roots.end());
        boundary.insert(late_represented_targets.begin(), late_represented_targets.end());
        boundary.insert(entry.value);
        boundary.erase(candidate_value);
        std::set<Address> cont_boundary = continuation_roots;
        cont_boundary.erase(candidate_value);
        const auto probe =
            discover_m68k_static_graph(late_target_address, limits, environment, boundary, cont_boundary);
        const bool entry_decoded = probe.decode_cache.find(late_target_address) != nullptr;
        const bool fatal_probe =
            (probe.primary_issue && is_fatal_probe_failure(*probe.primary_issue)) ||
            std::any_of(probe.secondary_issues.begin(), probe.secondary_issues.end(),
                        [&](const M68kDiscoveryIssue &issue) { return is_fatal_probe_failure(issue); });
        const bool truncated = candidate_walk_truncated(probe);
        if (!entry_decoded || fatal_probe || truncated) continue;
        const auto before_decoded = merged_decoded.size();
        const auto before_entries = block_entries.size();
        const auto before_edges = merged_edges.size();
        const auto before_frames = merged_frames.size();
        merge_root_result({StaticProgramRootKind::asynchronous_hardware, synthetic_root_counter++}, probe);
        if (late_represented_targets.insert(candidate_value).second)
          ++stitch_metrics.late_indirect_target_unit_count;
        if (merged_decoded.size() != before_decoded || block_entries.size() != before_entries ||
            merged_edges.size() != before_edges || merged_frames.size() != before_frames)
          representation_changed = true;
      }
      if (aggregation_conflict) {
        auto r = rejected(DirectFlowDiagnostic::startup_graph_mismatch, program);
        set_source(r, entry);
        return r;
      }

      if (representation_changed) {
        // The graph grew this round, so `provisional_sets` was NOT proven over
        // the same graph state its acceptance gate would now be evaluated on.
        // Commit nothing; recompute the proof over the expanded graph next
        // round. Representation growth alone keeps the fixed point running.
        round_changed = true;
      } else {
        // STABLE round: the graph was identical across `provisional_sets` and
        // the gate below. Only here may a Tier-1 relation be committed, and it
        // is still revalidated against every later expanded graph (top of loop)
        // before it keeps authorizing an edge / Stage-B traversal / retention.

        // --- Acceptance gate (EXISTING, unweakened).
        for (const auto &set : provisional_sets) {
          const auto source = set.source_instruction.source.address.value;
          const auto decoded = merged_decoded.find(source);
          if (decoded == merged_decoded.end() || decoded->second.kind != M68kInstructionKind::jmp ||
              indirect_by_source.contains(source) || !unproven_indirect_by_source.contains(source))
            continue;
          bool all_validated = !set.candidates.empty();
          for (const auto &candidate : set.candidates) {
            if (candidate.space != TargetAddressSpace::m68k_program ||
                environment.admit_target(candidate, M68kDiscoveryTargetRole::direct_call) ||
                !merged_decoded.contains(candidate.value) || !block_entries.contains(candidate.value)) {
              all_validated = false;
              break;
            }
          }
          if (!all_validated) continue;
          indirect_by_source.emplace(source, set);
          indirect_order.push_back(source);
          unproven_indirect_by_source.erase(source);
          unproven_indirect_order.erase(
              std::remove(unproven_indirect_order.begin(), unproven_indirect_order.end(), source),
              unproven_indirect_order.end());
          for (const auto &candidate : set.candidates) {
            const M68kStaticEdge edge{set.source_instruction, M68kStaticEdgeKind::indirect_branch, candidate,
                                      std::nullopt};
            const auto signature = EdgeSignature{source, static_cast<int>(edge.kind), candidate.value, false,
                                                 Address{}, Address{}};
            if (edge_signatures_seen.insert(signature).second) merged_edges.push_back(edge);
          }
          accepted_tier1_by_source.insert_or_assign(source, set);
          resolved_sources_all.insert(source);
          round_changed = true;
        }

        // --- Stage B (return-edge retention): re-run the identical
        // reachable-RTS trace, Tier-1-aware. It follows an accepted computed
        // JMP to its proven candidates and yields the RTS's
        // `return_to_continuation` edge under the ORIGINAL enclosing
        // (caller, callee, continuation) frame -- never a new frame for the
        // Tier-1 target itself.
        //
        // SEG-007-T231 correction: `accepted_tier1_by_source` tracks only the
        // relations THIS closure loop itself promoted through the Stage A
        // acceptance gate above (the An-indexed family `m68k_prove_stitched_
        // an_indirect_targets` revalidates each round) -- it deliberately
        // excludes a Tier-1 relation that arrived already proven from the
        // earlier cross-root merge (e.g. a `pc_index8` computed JMP whose
        // finite candidate set a single root's own walk already proved and
        // stitched into `indirect_by_source`/`merged_edges`/`block_entries`
        // together, before this loop ever ran). Passing the narrower map here
        // silently starved Stage B for every such early-merged source: while
        // tracing an existing call frame's body, `m68k_reachable_return_edges`
        // treats an unrecognized computed JMP as a dead end (see `return_
        // reachability_successors`'s `case jmp:` in static_program.cpp), so a
        // tail-dispatched candidate ending in RTS never received its `return_
        // to_continuation` edge and was later pruned as an orphan RTS by the
        // completed-prefix erase pass (SEG-007-T183/ADR-0028 Sec 8), even
        // though its own entry was validly decoded, block-registered, and
        // edge-connected by the very same per-root walk that proved the
        // relation. `indirect_by_source` is always a superset of `accepted_
        // tier1_by_source` (every Stage A acceptance inserts into both, in
        // the same round -- see above), so using it here is strictly
        // additive: it adds exactly the early-merged relations this
        // already-existing, already-Tier-1-aware trace was always documented
        // to cover, without changing which relations the revalidation loop
        // above re-proves (that loop still reads `accepted_tier1_by_source`
        // only, unchanged) and without synthesizing any new decode, edge,
        // frame, or candidate fact of its own.
        for (const auto &edge :
             m68k_reachable_return_edges(merged_decoded, merged_frames, indirect_by_source)) {
          const auto signature = EdgeSignature{
              edge.source_instruction.source.address.value, static_cast<int>(edge.kind), edge.target.value,
              edge.call.has_value(), edge.call ? edge.call->callee.value : Address{},
              edge.call ? edge.call->continuation.value : Address{}};
          if (edge_signatures_seen.insert(signature).second) {
            merged_edges.push_back(edge);
            round_changed = true;
          }
        }
      }

      ++closure_rounds;
      if (!round_changed) { converged = true; break; }
      if (closure_rounds >= kM68kLateIndirectClosureRoundCeiling) break;
    }
    stitch_metrics.late_indirect_closure_rounds = closure_rounds;
    stitch_metrics.late_indirect_closure_converged = converged;
    if (!converged) {
      // ADR-0031 fail-closed: no fixed point within the explicit ceiling.
      // Nothing proven is silently dropped -- the whole build fails closed
      // with a bounded diagnostic frontier (ADR-0028's fail-closed pattern).
      auto r = rejected(DirectFlowDiagnostic::discovery_budget_exhausted, program);
      set_source(r, entry);
      return r;
    }
    if (!resolved_sources_all.empty()) {
      const auto resolved_issue = [&](const M68kDiscoveryIssue &issue) {
        return issue.category == DirectFlowDiagnostic::reached_unresolved_direct_edge &&
               resolved_sources_all.contains(issue.address.value);
      };
      if (primary_issue && resolved_issue(*primary_issue)) {
        primary_issue.reset();
        primary_issue_root.reset();
      }
      std::vector<M68kDiscoveryIssue> kept_issues;
      std::vector<StaticProgramRootId> kept_roots;
      for (std::size_t i = 0; i < aggregated_secondary_issues.size(); ++i) {
        if (resolved_issue(aggregated_secondary_issues[i])) continue;
        kept_issues.push_back(std::move(aggregated_secondary_issues[i]));
        kept_roots.push_back(aggregated_secondary_issue_roots[i]);
      }
      aggregated_secondary_issues = std::move(kept_issues);
      aggregated_secondary_issue_roots = std::move(kept_roots);
    }
  }

  const auto &decoded_by_address = merged_decoded;
  analysis.static_edges = merged_edges;
  analysis.static_frames = merged_frames;
  // SEG-007-T124 / ADR-0009: one retained fact per source instruction whose
  // computed/indirect control EA discovery proved a finite target set for.
  std::vector<M68kIndirectTargetEaSet> merged_indirect_target_ea_sets;
  merged_indirect_target_ea_sets.reserve(indirect_order.size());
  for (const auto &key : indirect_order) merged_indirect_target_ea_sets.push_back(indirect_by_source.at(key));
  analysis.indirect_target_ea_sets = merged_indirect_target_ea_sets;
  // SEG-007-T174 / ADR-0024: the weaker Tier-2-eligible sibling set.
  std::vector<M68kUnprovenIndirectControlEaSet> merged_unproven_indirect_control_ea_sets;
  merged_unproven_indirect_control_ea_sets.reserve(unproven_indirect_order.size());
  for (const auto &key : unproven_indirect_order)
    merged_unproven_indirect_control_ea_sets.push_back(unproven_indirect_by_source.at(key));
  analysis.unproven_indirect_control_ea_sets = merged_unproven_indirect_control_ea_sets;
  // SEG-007-T047 / ADR-0020 §6: retain the build-time-resolved IRQ6 autovector
  // handler entry. Emission (genesis.cpp) only writes runtime->irq6_handler_*
  // when the handler block is also in the emitted dispatch set.
  if (irq6_handler_entry_value)
    analysis.irq6_handler_entry = M68kProgramAddress{TargetAddressSpace::m68k_program, *irq6_handler_entry_value};
  // SEG-007-T222 / ADR-0037: retain the build-time-resolved vector-5 handler
  // entry, mirroring `irq6_handler_entry` exactly.
  if (divide_by_zero_handler_entry_value)
    analysis.divide_by_zero_handler_entry =
        M68kProgramAddress{TargetAddressSpace::m68k_program, *divide_by_zero_handler_entry_value};
  // SEG-007-T174 / ADR-0024: every external code-entry candidate that
  // discovery actually independently decoded (not merely attempted as a
  // seed -- `block_entries` unconditionally records every seed's own bare
  // address before its own walk even starts, so membership there alone
  // would not distinguish a validated candidate from a false positive), in
  // the hints file's own sorted/deduplicated order. A candidate that never
  // decoded/mapped (a false positive pointing at data, an unmapped address,
  // or an invalid decode) simply never appears in `merged_decoded` and is
  // silently excluded here -- it never becomes a reachability root, exactly
  // like it never became a retained instruction.
  for (const auto &candidate : program.external_code_entry_candidates)
    if (merged_decoded.contains(candidate.address))
      analysis.validated_code_entry_candidate_roots.push_back(
          M68kProgramAddress{TargetAddressSpace::m68k_program, candidate.address});

  // SEG-007-T183 / ADR-0028 §8: the semantic partition-boundary address set
  // -- every validated destination the aggregated multi-unit static graph
  // may legitimately target independent of the bounded diagnostic frontier
  // report. Built here, after `admitted_units`, `continuation_roots`,
  // `synthesized_control_roots`, `analysis.validated_code_entry_candidate_
  // roots`, `analysis.irq6_handler_entry`, and `analysis.indirect_target_ea_
  // sets` are all finalized, so it reflects every already-validated fact
  // this same walk produced. Carries only addresses -- no instructions,
  // edges, or blocks -- and is never consulted to admit a NEW address; it
  // only widens which already-admitted-elsewhere address a retained block's
  // outgoing edge may legitimately target.
  analysis.semantic_partition_boundary_addresses = admitted_units;
  analysis.semantic_partition_boundary_addresses.insert(continuation_roots.begin(), continuation_roots.end());
  analysis.semantic_partition_boundary_addresses.insert(synthesized_control_roots.begin(),
                                                          synthesized_control_roots.end());
  for (const auto &root : analysis.validated_code_entry_candidate_roots)
    if (root.space == TargetAddressSpace::m68k_program)
      analysis.semantic_partition_boundary_addresses.insert(root.value);
  if (analysis.irq6_handler_entry && analysis.irq6_handler_entry->space == TargetAddressSpace::m68k_program)
    analysis.semantic_partition_boundary_addresses.insert(analysis.irq6_handler_entry->value);
  if (analysis.divide_by_zero_handler_entry &&
      analysis.divide_by_zero_handler_entry->space == TargetAddressSpace::m68k_program)
    analysis.semantic_partition_boundary_addresses.insert(analysis.divide_by_zero_handler_entry->value);
  for (const auto &set : analysis.indirect_target_ea_sets)
    for (const auto &candidate : set.candidates)
      if (candidate.space == TargetAddressSpace::m68k_program)
        analysis.semantic_partition_boundary_addresses.insert(candidate.value);
  // This deliberately exists only for project-authored synthetic fixtures.
  // It observes the same already-decoded boundary seam as production facts,
  // without creating an input-format or runtime authority path.
  if (program.image.source_id.starts_with("synthetic/"))
    for (const auto boundary : program.synthetic_semantic_partition_boundaries_for_test)
      if (boundary.space == TargetAddressSpace::m68k_program && merged_decoded.contains(boundary.value))
        analysis.semantic_partition_boundary_addresses.insert(boundary.value);
  stitch_metrics.semantic_partition_boundary_count =
      static_cast<std::uint32_t>(analysis.semantic_partition_boundary_addresses.size());

  // Translate the CPU boundary's own typed primary/secondary issues back
  // into this profile's established FrontendRejected/frontier-access shapes.
  // See translate_m68k_discovery_issue's own doc comment.
  std::optional<FrontendRejected> failure;
  std::optional<M68kMemoryAccessRequest> frontier_access;
  if (primary_issue) {
    failure = translate_m68k_discovery_issue(program, *primary_issue);
    if (primary_issue->access) {
      const auto &access = *primary_issue->access;
      frontier_access = M68kMemoryAccessRequest{access.address, access.width, access.direction, access.provenance};
    }
  }
  // SEG-007-T064: each best-effort sibling exploration inside
  // discover_m68k_static_graph's own walk may contribute one additional
  // candidate runtime exit, captured here as its own (failure, frontier_
  // access) pair, distinct from the primary `failure`/`frontier_access`
  // above. partial_or_rejection below is the sole consumer.
  std::vector<std::pair<FrontendRejected, std::optional<M68kMemoryAccessRequest>>> secondary_failures;
  secondary_failures.reserve(aggregated_secondary_issues.size());
  for (const auto &issue : aggregated_secondary_issues) {
    std::optional<M68kMemoryAccessRequest> access;
    if (issue.access) {
      access = M68kMemoryAccessRequest{issue.access->address, issue.access->width, issue.access->direction,
                                        issue.access->provenance};
    }
    secondary_failures.emplace_back(translate_m68k_discovery_issue(program, issue), access);
  }

  // SEG-007-T064: the bounded set of allowed non-retained edge targets --
  // the primary failure's own source address plus every best-effort
  // sibling's own source address -- in place of the single `frontier_
  // address` build_analysis's pruning/edge-repopulation logic used before
  // this task. Restricted to candidates that carry `diagnostic.provenance`:
  // every eligibility path (the four precise classes and known_but_
  // unemitted_target alike) unconditionally requires it (runtime_frontier_
  // eligible's own top-level structural gate), so a provenance-less
  // candidate (only reachable today via a discovery-budget-exhaustion-style
  // rejection deep inside a sibling's own further exploration) could never
  // become a represented exit in any form -- treating its address as a safe
  // edge target would let build_analysis retain a block whose edge can never
  // actually be lowered, so it is excluded here and that block's own
  // retention is decided exactly as it was before this task (conservatively
  // pruned unless retained by some other path). This same set is also
  // threaded into every runtime_frontier_eligible call below (both
  // build_analysis's own construction and partial_or_rejection's per-exit
  // checks), so both agree on exactly the same allowed-edge-target set.
  // SEG-007-T184 / ADR-0009 / ADR-0024 aggregation-consistency correction: a
  // `M68kUnprovenIndirectControlEaSet` (Tier-2) fact is, by ADR-0024's own
  // construction, ALWAYS recorded at exactly the address where that fact's
  // OWN owning per-root walk hit `reached_unresolved_direct_edge` on a
  // computed control-transfer terminal (`libs/codegen/c11/src/frontend.cpp`'s own
  // documented invariant: "by construction, `exclude_frontier_instruction`
  // above always excludes [it] from ever becoming part of a retained static
  // block"). Before this correction, this set was built ONLY from `failure`/
  // `aggregated_secondary_issues` -- populated from a walk's own
  // `primary_issue`/`secondary_issues`. ADR-0026 Phase 2 intentionally DROPS
  // a non-authoritative admitted-unit/candidate root's own `primary_issue`/
  // `secondary_issues` (an offline-inventory false positive must not fail the
  // whole build), but that drop also silently discarded this address from
  // `candidate_frontier_addresses` even when a real Tier-2 fact for it
  // survived the drop untouched (`merge_root_result` never filters
  // `unproven_indirect_control_ea_sets`). The result: a computed
  // control-transfer terminal that is ALSO an admitted-unit's own entry point
  // (so it can never satisfy Tier-1's entry-seeded finite-value producer, by
  // that producer's own per-root-scoped construction) stayed eligible to
  // become an ordinary retained static-block terminal, later hitting C4's
  // unconditional Tier-1 `indirect_target_ea_sets` requirement with no
  // fallback -- the exact "invalid C4 indirect target set" rejection. Folding
  // every already-computed Tier-2 fact's own source address in here restores
  // the documented invariant unconditionally, independent of which root's
  // walk happened to keep or drop its own primary/secondary issue. This adds
  // no new fact, admission rule, or edge kind; it only lets an already-
  // computed Tier-2 fact reach the SAME single allowed-edge-target set every
  // other frontier address already flows through.
  const auto compute_candidate_frontier_addresses = [&]() -> std::set<Address> {
    std::set<Address> addresses;
    if (failure && failure->source_address && failure->provenance) addresses.insert(failure->source_address->value);
    for (const auto &secondary : secondary_failures)
      if (secondary.first.source_address && secondary.first.provenance)
        addresses.insert(secondary.first.source_address->value);
    for (const auto &unproven : analysis.unproven_indirect_control_ea_sets)
      addresses.insert(unproven.source_instruction.source.address.value);
    return addresses;
  };
  // SEG-007-T182 / ADR-0028: cross-unit convergence-point block splitting.
  // Independently-walked stitched units (offline admitted, synthesized
  // resolved-control-target, synthesized fallthrough-continuation, reset / IRQ6
  // roots) can converge onto a common straight-line tail that no single unit
  // registered as a block entry. Without an explicit leader there,
  // build_analysis's per-entry straight-line reconstruction emits two retained
  // blocks that share every instruction from the convergence address onward and
  // runtime_frontier_eligible's per-block retained-instruction uniqueness guard
  // rejects the whole prefix. Promote to a block entry every decoded
  // instruction that is either (a) the straight-line fallthrough successor of
  // two or more distinct decoded instructions, or (b) simultaneously a
  // straight-line fallthrough successor and a direct control-transfer edge
  // target. The reconstructed blocks then partition the instruction space with
  // no shared instruction address across two retained blocks. Deterministic and
  // traversal-order independent: derived only from the already-deterministic
  // merged decode map (`decode_order`) and merged edge list. Identical
  // overlapping decode was already deduplicated as agreement by
  // merge_root_result / decoded_matches; a genuine decode / edge / target-set /
  // completion conflict already set `aggregation_conflict` and fails closed
  // below as `startup_graph_mismatch`. No discovery / block / seed / round /
  // frontier ceiling is consulted or raised here.
  {
    const auto is_block_transfer = [](M68kInstructionKind kind) {
      return kind == M68kInstructionKind::bne_short || kind == M68kInstructionKind::bra_short ||
             kind == M68kInstructionKind::rts || kind == M68kInstructionKind::rte ||
             kind == M68kInstructionKind::jmp || kind == M68kInstructionKind::jsr ||
             kind == M68kInstructionKind::branch || kind == M68kInstructionKind::bsr ||
             kind == M68kInstructionKind::dbcc;
    };
    std::map<Address, std::set<Address>> fallthrough_predecessors;
    for (const auto addr : decode_order) {
      const auto found = decoded_by_address.find(addr);
      if (found == decoded_by_address.end() || is_block_transfer(found->second.kind)) continue;
      const auto next_pc = static_cast<Address>(addr + found->second.provenance.length.value);
      if (decoded_by_address.find(next_pc) == decoded_by_address.end()) continue;
      fallthrough_predecessors[next_pc].insert(addr);
    }
    std::set<Address> control_edge_targets;
    for (const auto &edge : analysis.static_edges)
      if (edge.target.space == TargetAddressSpace::m68k_program)
        control_edge_targets.insert(edge.target.value);
    std::vector<Address> convergence_entries;
    for (const auto addr : decode_order) {
      if (block_entries.contains(addr)) continue;
      const auto pred = fallthrough_predecessors.find(addr);
      if (pred == fallthrough_predecessors.end()) continue;
      if (pred->second.size() >= 2U || control_edge_targets.contains(addr))
        convergence_entries.push_back(addr);
    }
    for (const auto addr : convergence_entries)
      if (block_entries.insert(addr).second) block_entry_order.push_back(addr);
  }
  enum class CompletedPrefixEraseReason : std::uint8_t {
    unreachable,
    orphan_rts,
    unresolved_computed_control,
    unsafe_outgoing_relation,
  };
  struct CompletedPrefixUnsafeRelation {
    Address source_entry{};
    M68kStaticEdge edge{};
  };
  struct CompletedPrefixEraseFact {
    Address entry{};
    CompletedPrefixEraseReason reason{};
    std::vector<CompletedPrefixUnsafeRelation> unsafe_relations{};
  };
  struct CompletedPrefixEraseTransaction {
    std::vector<CompletedPrefixEraseFact> facts{};
  };
  const auto build_analysis = [&](bool completed_blocks_only, bool exclude_frontier_instruction,
                                  CompletedPrefixEraseTransaction *erase_transaction = nullptr,
                                  const std::set<Address> *provisional_frontier_addresses = nullptr,
                                  std::set<Address> *unreachable_retained_entries = nullptr,
                                  // SEG-007-T242 / ADR-0039 ("successor freeze 1"): this parameter's
                                  // sole consumer was the reachability-based erase condition removed
                                  // below. It is kept, unused, purely so every existing call site's
                                  // argument list stays byte-identical; it has no remaining effect.
                                  [[maybe_unused]] const std::set<Address> *provisional_entry_addresses = nullptr)
      -> std::optional<FrontendAnalysis> {
    if (erase_transaction != nullptr) erase_transaction->facts.clear();
    const auto candidate_frontier_addresses = compute_candidate_frontier_addresses();
    const auto represented_nonretained_target = [&](Address address) {
      return candidate_frontier_addresses.contains(address) ||
             (provisional_frontier_addresses != nullptr && provisional_frontier_addresses->contains(address));
    };
    FrontendAnalysis prefix = analysis;
    const auto discovered_edges = prefix.static_edges;
    const auto discovered_indirect_target_ea_sets = prefix.indirect_target_ea_sets;
    prefix.decoded.clear();
    prefix.ir.clear();
    prefix.static_blocks.clear();
    prefix.static_edges.clear();
    for (const auto entry_addr : block_entry_order) {
      std::vector<InstructionProvenance> block_instructions;
      Address pc = entry_addr;
      bool complete = true;
      for (;;) {
        const auto decoded = decoded_by_address.find(pc);
        if (decoded == decoded_by_address.end()) { complete = false; break; }
        if (exclude_frontier_instruction &&
            candidate_frontier_addresses.contains(decoded->second.provenance.source.address.value)) {
          complete = false;
          break;
        }
        block_instructions.push_back(decoded->second.provenance);
        const auto next_pc = static_cast<Address>(pc + decoded->second.provenance.length.value);
        const bool is_transfer = decoded->second.kind == M68kInstructionKind::bne_short ||
                                 decoded->second.kind == M68kInstructionKind::bra_short ||
                                 decoded->second.kind == M68kInstructionKind::rts ||
                                 decoded->second.kind == M68kInstructionKind::rte ||
                                 decoded->second.kind == M68kInstructionKind::jmp ||
                                 decoded->second.kind == M68kInstructionKind::jsr ||
                                 decoded->second.kind == M68kInstructionKind::branch ||
                                 decoded->second.kind == M68kInstructionKind::bsr ||
                                 decoded->second.kind == M68kInstructionKind::dbcc;
        if (is_transfer) {
          // A terminal direct branch/DBcc/BSR whose discovery never recorded a single outgoing edge belongs to
          // a walk that did not complete (its decoded bytes survived, its control edges did not). Retaining it
          // as a "completed" block would hand C4 a branch terminal with no successor (an incomplete static edge
          // that rejects the entire program as soon as any retained code reaches it), so it is incomplete.
          const auto kind = decoded->second.kind;
          const bool edge_bearing_terminal =
              kind == M68kInstructionKind::bne_short || kind == M68kInstructionKind::bra_short ||
              kind == M68kInstructionKind::branch || kind == M68kInstructionKind::bsr ||
              kind == M68kInstructionKind::dbcc;
          if (completed_blocks_only && edge_bearing_terminal &&
              std::none_of(discovered_edges.begin(), discovered_edges.end(), [&](const M68kStaticEdge &edge) {
                return edge.source_instruction.source.address.value == pc;
              }))
            complete = false;
          break;
        }
        // SEG-007-T040 (C3): a straight-line block whose very next address is
        // the known frontier (the one instruction discovery separately
        // failed on) is exactly as cleanly terminated as one merging into an
        // already-registered block entry -- the frontier address was never
        // itself eligible to become a block entry (it never decoded), so
        // without this it falls through to the "decoded not found" branch
        // above on the next iteration and discards this whole block as
        // though it were merely truncated/incomplete, even though every
        // instruction actually retained here decoded and lifted cleanly.
        // Reuse the exact same fallthrough-edge shape the block-entry merge
        // case below already produces; runtime_frontier_eligible already
        // accepts a fallthrough edge whose target equals the frontier
        // address (see its own edge-target check), so this adds no new
        // eligibility rule, only the missing edge/completion that lets an
        // already-eligible shape actually reach it.
        const bool reaches_frontier = exclude_frontier_instruction &&
                                       candidate_frontier_addresses.contains(next_pc);
        // SEG-007-T185 / ADR-0028 §8 (boundary-retention-vs-representation
        // batch): a straight-line run also terminates cleanly -- emitting the
        // exact same representable `fallthrough` edge the block-entry-merge
        // and known-frontier cases above already produce -- when its ordinary
        // PC advance lands on a validated semantic partition boundary. Before
        // this batch this decision consulted only `block_entries` and the
        // bounded diagnostic `candidate_frontier_addresses` set, so a boundary
        // address reachable only as a single straight-line fallthrough
        // successor (not registered as a block entry, not carrying a live
        // Tier-1/Tier-2 discovery-issue fact) was silently absorbed past --
        // producing a retained block C4 then correctly rejects for lacking a
        // terminal control transfer. The completed-prefix erase pass's own
        // edge-safety check and C4's structural edge-target gate already treat
        // this same target as a legitimate non-retained edge destination
        // (SEG-007-T183); this makes block construction stop consistently with
        // what that downstream logic already accepts. Unconditional (not gated
        // on `exclude_frontier_instruction`): boundary retention is
        // independent of the bounded diagnostic frontier projection, exactly
        // as the erase pass already treats it. Adds no new edge kind, frontier
        // class, or admitted address.
        const bool reaches_semantic_partition_boundary =
            is_semantic_partition_boundary_address(analysis, next_pc);
        if (block_entries.contains(next_pc) || reaches_frontier || reaches_semantic_partition_boundary) {
          prefix.static_edges.push_back({decoded->second.provenance, M68kStaticEdgeKind::fallthrough,
                                          {TargetAddressSpace::m68k_program, next_pc}, std::nullopt});
          break;
        }
        pc = next_pc;
      }
      if (!complete) {
        if (completed_blocks_only) continue;
        return std::nullopt;
      }
      prefix.static_blocks.push_back(
          {BlockId{{TargetAddressSpace::m68k_program, entry_addr}}, std::move(block_instructions)});
    }
    const auto block_edges = prefix.static_edges;
    if (completed_blocks_only) {
      // A partial prefix is independently complete: iteratively discard a
      // completed block when one of its outgoing edges reaches neither another
      // retained completed block nor a candidate frontier/boundary address.
      // Iteration is required because dropping one block can invalidate a
      // predecessor that targeted it.
      //
      // ADR-0014 Decision §1b (M1b) note: this erase pass keys off each
      // retained block's own OUTGOING edges only; a JSR/BSR call site's own
      // outgoing edge always targets its callee (an M68kStaticEdgeKind::
      // direct_call/indirect_call edge), never its continuation -- there is
      // no `M68kStaticEdgeKind` connecting a call site directly to its own
      // continuation (that address is reached only through the caller's own
      // successor block-entry registration, plus, when the callee's RTS was
      // admitted, a separately synthesized `return_to_continuation` edge
      // sourced at the RTS). So a retained continuation block is never
      // erased BY this pass for lacking a call-to-continuation edge; it
      // survives here as its own independently registered block entry
      // exactly as ADR-0014 Context §A describes ("blocks downstream of an
      // erased block ... become unreachable from the entry" without being
      // erased). What this pass DOES need from ADR-0014 Decision §2 is that
      // every genuinely admitted block's outgoing edge target is either a
      // retained block entry or a member of `candidate_frontier_addresses`
      // (the bounded boundary-address set the ceiling handler enumerates
      // from exactly this same `edges_`/`block_entries_` traversal state) --
      // a property Decision §2's construction guarantees by keying the
      // boundary set off admitted-block edge targets directly, so this erase
      // pass never needs its own copy of the M1b relation to avoid
      // mis-erasing a legitimately retained ancestor. The actual entry-
      // connectivity gap M1b closes -- a retained continuation being
      // provably REACHABLE from the seed entry -- is enforced by the
      // dedicated breadth-first walk in `runtime_frontier_eligible` (both
      // mirrors) below, which is where the M1b relation is applied.
      std::vector<M68kStaticEdge> all_edges = block_edges;
      all_edges.insert(all_edges.end(), discovered_edges.begin(), discovered_edges.end());
      // SEG-007-T183 / ADR-0028 §8 instrumentation: the completed-blocks-only
      // prefix's own block count immediately before this erase pass runs.
      stitch_metrics.retained_block_count_before_pruning =
          static_cast<std::uint32_t>(prefix.static_blocks.size());
      const auto block_contains = [](const M68kStaticBlock &block, Address address) {
        return std::any_of(block.instructions.begin(), block.instructions.end(), [&](const auto &instruction) {
          return instruction.source.address.value == address;
        });
      };
      // SEG-007-T183 / ADR-0028 §8: entry-connectivity pruning, mirroring
      // `runtime_frontier_eligible`'s own ADR-0014 M1b breadth-first walk
      // exactly (same seeds: ingress, IRQ6 handler entry, validated
      // candidate roots; same successor relation: every retained block's own
      // static edges plus the M1b call->continuation relation). At large
      // multi-unit scale, widening edge-target safety (above) can leave a
      // block that is itself edge-safe but only reachable through ANOTHER
      // block whose own edge was unsafe (a caller that never completed, so
      // its own call edge/frame never survive) -- a self-consistent but
      // entirely disconnected island. Computed fresh each fixpoint round
      // below (over the CURRENT `prefix.static_blocks`), since pruning one
      // island can strand a block that was only reachable through it.
      const auto currently_unreachable_entries = [&]() -> std::set<Address> {
        std::map<Address, std::vector<Address>> local_successors;
        std::set<Address> current_entries;
        for (const auto &block : prefix.static_blocks) current_entries.insert(block.id.entry.value);
        // Only edges whose source instruction belongs to a CURRENTLY retained
        // block contribute a successor relation -- exactly like the edge-
        // safety pass above, restated per this round's own block set.
        std::set<Address> retained_instruction_addresses;
        for (const auto &block : prefix.static_blocks)
          for (const auto &instruction : block.instructions)
            retained_instruction_addresses.insert(instruction.source.address.value);
        for (const auto &edge : all_edges) {
          if (!retained_instruction_addresses.contains(edge.source_instruction.source.address.value)) continue;
          // SEG-007-T183 / ADR-0028 §8: mirror the exact same dangling-
          // return-edge exclusion the final `discovered_edges` filter below
          // applies -- a `return_to_continuation` edge sourced at a retained
          // RTS does not represent a real reachable path when its own call's
          // CALLER instruction is not retained (that call was never actually
          // admitted). Excluding it here too keeps this reachability pass
          // consistent with the edge set `runtime_frontier_eligible` will
          // ultimately observe.
          if (edge.kind == M68kStaticEdgeKind::return_to_continuation && edge.call &&
              !retained_instruction_addresses.contains(edge.call->caller.source.address.value))
            continue;
          local_successors[edge.source_instruction.source.address.value].push_back(edge.target.value);
        }
        // ADR-0014 Decision §1b (M1b): a retained call reaches its own
        // continuation independent of whether the callee's RTS itself
        // survived, exactly mirroring `runtime_frontier_eligible`'s own use
        // of this relation.
        for (const auto &frame : prefix.static_frames)
          if (retained_instruction_addresses.contains(frame.call.caller.source.address.value))
            local_successors[frame.call.caller.source.address.value].push_back(
                static_cast<Address>(frame.call.caller.source.address.value + frame.call.caller.length.value));
        std::map<Address, const M68kStaticBlock *> local_blocks_by_entry;
        for (const auto &block : prefix.static_blocks) local_blocks_by_entry.emplace(block.id.entry.value, &block);
        std::set<Address> reachable;
        std::vector<Address> pending;
        const auto seed = [&](Address value) {
          if (reachable.insert(value).second) pending.push_back(value);
        };
        if (prefix.startup_ingress && prefix.startup_ingress->entry.space == TargetAddressSpace::m68k_program)
          seed(prefix.startup_ingress->entry.value);
        if (analysis.irq6_handler_entry && analysis.irq6_handler_entry->space == TargetAddressSpace::m68k_program)
          seed(analysis.irq6_handler_entry->value);
        if (analysis.divide_by_zero_handler_entry &&
            analysis.divide_by_zero_handler_entry->space == TargetAddressSpace::m68k_program)
          seed(analysis.divide_by_zero_handler_entry->value);
        for (const auto &root : analysis.validated_code_entry_candidate_roots)
          if (root.space == TargetAddressSpace::m68k_program) seed(root.value);
        // Mirrors `runtime_frontier_eligible`'s own walk exactly: `pending`
        // holds BLOCK ENTRY addresses, and a block's own outgoing edge is
        // typically sourced at its TERMINAL instruction, not its entry --
        // so every instruction in the block (not just the entry address
        // itself) must be checked against `local_successors`.
        for (std::size_t index = 0; index < pending.size(); ++index) {
          const auto block = local_blocks_by_entry.find(pending[index]);
          if (block == local_blocks_by_entry.end()) continue;
          for (const auto &instruction : block->second->instructions) {
            const auto found = local_successors.find(instruction.source.address.value);
            if (found == local_successors.end()) continue;
            for (const auto target : found->second) seed(target);
          }
        }
        std::set<Address> unreachable;
        for (const auto entry : current_entries)
          if (!reachable.contains(entry)) unreachable.insert(entry);
        return unreachable;
      };
      bool changed = false;
      do {
        changed = false;
        std::set<Address> retained_entries;
        for (const auto &block : prefix.static_blocks) retained_entries.insert(block.id.entry.value);
        const auto original_count = prefix.static_blocks.size();
        std::set<Address> retained_instruction_addresses_now;
        for (const auto &b : prefix.static_blocks)
          for (const auto &i : b.instructions) retained_instruction_addresses_now.insert(i.source.address.value);
        // SEG-007-T242 / ADR-0039 ("successor freeze 1"): missing static graph
        // reachability/ownership from the known roots is no longer, by
        // itself, treated as an intrinsic safety failure. An independently
        // walked block that is otherwise mapped, bounds-safe, aligned,
        // correctly decoded, non-conflicting, complete, and lowerable, and
        // that has no unsafe outgoing edge, keeps its executable identity
        // even when the aggregate forward flood from the known roots never
        // reaches its entry. `currently_unreachable_entries()` (below, and
        // still called unconditionally after this loop for the unchanged
        // `unreachable_retained_entries` diagnostic out-parameter) and the
        // ADR-0028 §8.2 `direct_control_target_entries` exception that used
        // to carve out a narrower reachability-adjacent allowance are both
        // retired here: the erase-based use of "no proven static graph
        // reachability" is removed entirely rather than narrowed further.
        // The orphan-RTS check, the unresolved-computed-control check, and
        // the outgoing-edge-safety check below are untouched and still fail
        // closed exactly as before -- only the standalone reachability erase
        // is gone.
        std::erase_if(prefix.static_blocks, [&](const M68kStaticBlock &block) {
          const auto record_erase = [&](CompletedPrefixEraseReason reason,
                                        std::vector<CompletedPrefixUnsafeRelation> relations = {}) {
            if (erase_transaction != nullptr)
              erase_transaction->facts.push_back({block.id.entry.value, reason, std::move(relations)});
            return true;
          };
          // SEG-007-T183 / ADR-0028 §8: a block whose terminal instruction is
          // RTS but has no surviving `return_to_continuation` edge (every
          // candidate one was dropped above because its own call's caller is
          // not retained -- an orphaned return with no coherent runtime
          // target) is unrepresentable exactly like a block that never
          // completed: C4's own per-block completeness check requires at
          // least one such edge for an RTS terminal, and there is no static
          // fact anywhere in this program describing where an uncalled RTS
          // should transfer control. Prune it here rather than let it reach
          // that later unconditional rejection.
          const auto &terminal = block.instructions.back();
          const auto terminal_decoded = decoded_by_address.find(terminal.source.address.value);
          if (terminal_decoded != decoded_by_address.end() &&
              terminal_decoded->second.kind == M68kInstructionKind::rts) {
            const bool has_live_return_edge = std::any_of(
                all_edges.begin(), all_edges.end(), [&](const M68kStaticEdge &edge) {
                  return edge.kind == M68kStaticEdgeKind::return_to_continuation &&
                         edge.source_instruction.source.address.value == terminal.source.address.value &&
                         edge.call &&
                         retained_instruction_addresses_now.contains(edge.call->caller.source.address.value) &&
                         // SEG-007-T185 / ADR-0028 §8.2: a live return path also
                         // needs its continuation block still retained -- a
                         // return edge whose continuation was pruned is dropped
                         // from the final edge set below, so it cannot count
                         // here. Keeps this orphan-RTS gate consistent with the
                         // edge set C4 ultimately observes; a callee retaining at
                         // least one representable return path still survives.
                         (analysis.semantic_partition_boundary_addresses.empty() ||
                          retained_entries.contains(edge.target.value) ||
                          represented_nonretained_target(edge.target.value) ||
                          is_semantic_partition_boundary_address(analysis, edge.target.value));
            });
            // SEG-007-T236: assisted/runtime-routed prefixes may retain an RTS
            // without a statically owned return edge when an ordinary retained
            // control edge reaches the RTS block.  The generated RTS still
            // pops the real PC and C4 independently requires a non-empty
            // generic return-target authority before it emits this block.  No
            // frame or return edge is synthesized here.  Keep raw/unannotated
            // profiles unchanged by using the existing offline-inventory gate.
            const bool has_prospective_runtime_return_authority =
                std::any_of(prefix.static_frames.begin(), prefix.static_frames.end(),
                            [&](const M68kStaticFrame &frame) {
                  return retained_entries.contains(frame.call.continuation.value) ||
                         represented_nonretained_target(frame.call.continuation.value);
                }) ||
                std::any_of(analysis.unproven_indirect_control_ea_sets.begin(),
                            analysis.unproven_indirect_control_ea_sets.end(),
                            [&](const M68kUnprovenIndirectControlEaSet &set) {
                  return set.is_call;
                });
            const bool has_ordinary_retained_predecessor =
                stitch_metrics.offline_candidate_count != 0U &&
                has_prospective_runtime_return_authority &&
                std::any_of(all_edges.begin(), all_edges.end(), [&](const M68kStaticEdge &edge) {
                  return edge.kind != M68kStaticEdgeKind::return_to_continuation &&
                         edge.target.space == TargetAddressSpace::m68k_program &&
                         edge.target.value == block.id.entry.value &&
                         retained_instruction_addresses_now.contains(
                             edge.source_instruction.source.address.value);
                });
            if (!has_live_return_edge && !has_ordinary_retained_predecessor) {
              return record_erase(CompletedPrefixEraseReason::orphan_rts);
            }
          }
          // SEG-007-T243 / ADR-0039 ("successor freeze 2"): a block whose
          // terminal is a computed-control JMP/JSR (`pc_index8` or a bare
          // register-indirect `(An)` control EA) but for which per-root
          // discovery proved neither a finite Tier-1 `M68kIndirectTargetEaSet`
          // nor recorded a Tier-2-eligible `M68kUnprovenIndirectControlEaSet`
          // fact no longer unconditionally erases the whole containing block.
          // T194 previously hard-deleted it for lack of a static proof even
          // when the block itself decoded, lifted, and terminated cleanly and
          // was genuinely reached by other retained code -- a non-monotonic
          // loss of representation. This check now mirrors the orphan-RTS
          // check immediately above exactly: only a block with no live
          // incoming edge from any currently-retained source (a genuinely
          // unreachable/speculative terminal -- e.g. an untrusted offline
          // candidate root that nothing else in the accepted prefix ever
          // reaches) is still pruned here. A block reached by at least one
          // live retained predecessor keeps its representation; C4/emission
          // (libs/codegen/c11/src/frontend.cpp's `emit_m68k_general_startup_runtime_c`)
          // routes that retained terminal through its own typed fail-closed
          // stop before any architectural mutation, exactly like the
          // existing incomplete-Tier-1-candidate-set case.
          if (terminal_decoded != decoded_by_address.end() &&
              (terminal_decoded->second.kind == M68kInstructionKind::jmp ||
               terminal_decoded->second.kind == M68kInstructionKind::jsr)) {
            const auto &terminal_ea = terminal_decoded->second.source_ea;
            const bool terminal_is_computed_control_ea =
                terminal_ea.mode == M68kEaMode::pc_index8 ||
                (terminal_ea.mode == M68kEaMode::address_indirect && terminal_ea.displacement == 0 &&
                 terminal_ea.extension_words == 0U);
            if (terminal_is_computed_control_ea) {
              const bool has_proven_indirect_target_set =
                  std::any_of(discovered_indirect_target_ea_sets.begin(), discovered_indirect_target_ea_sets.end(),
                              [&](const M68kIndirectTargetEaSet &set) {
                                return set.source_instruction.source.address.value == terminal.source.address.value;
              });
              const bool has_unproven_tier2_fact =
                  std::any_of(analysis.unproven_indirect_control_ea_sets.begin(),
                              analysis.unproven_indirect_control_ea_sets.end(),
                              [&](const M68kUnprovenIndirectControlEaSet &set) {
                                return set.source_instruction.source.address.value == terminal.source.address.value;
              });
              if (!has_proven_indirect_target_set && !has_unproven_tier2_fact) {
                const bool has_live_predecessor_edge = std::any_of(
                    all_edges.begin(), all_edges.end(), [&](const M68kStaticEdge &edge) {
                  return edge.target.space == TargetAddressSpace::m68k_program &&
                         edge.target.value == block.id.entry.value &&
                         retained_instruction_addresses_now.contains(edge.source_instruction.source.address.value);
                });
                if (!has_live_predecessor_edge) {
                  return record_erase(CompletedPrefixEraseReason::unresolved_computed_control);
                }
              }
            }
          }
          std::vector<CompletedPrefixUnsafeRelation> unsafe_relations;
          for (const auto &edge : all_edges) {
            if (!block_contains(block, edge.source_instruction.source.address.value)) continue;
            // SEG-007-T185 / ADR-0028 §8.2 (direct-control target preservation): a
            // `return_to_continuation` edge sourced at this block's own RTS whose
            // continuation block was pruned in an earlier fixpoint round is a dead
            // return sub-path, NOT an unsafe outgoing edge. The final
            // `discovered_edges` rebuild below already drops exactly this dangling
            // return edge (see the mirrored guard next to the `!source_retained
            // (edge.call->caller)` drop), and the orphan-RTS check above still
            // fails the block closed when it has NO live return edge at all -- so
            // a callee with at least one still-retained continuation must not be
            // dragged down (taking every OTHER caller's call site with it) just
            // because one shared caller's continuation unit was independently
            // pruned. Without this, a single pruned continuation cascades through
            // the shared callee to a program-wide collapse of that call tree,
            // leaving generated `BSR`/`JSR` sites transferring to a callee with no
            // emitted block (`genesis_internal_dispatch_inconsistency_stop`).
            // Gated to the multi-unit offline route (the only route that prunes
            // continuation units); the raw single-unit route stays byte-identical.
            if (edge.kind == M68kStaticEdgeKind::return_to_continuation &&
                !analysis.semantic_partition_boundary_addresses.empty() &&
                !retained_entries.contains(edge.target.value) &&
                !represented_nonretained_target(edge.target.value) &&
                !is_semantic_partition_boundary_address(analysis, edge.target.value))
              continue;
            // SEG-007-T183 / ADR-0028 §8: a source block also survives when
            // its outgoing edge targets a validated semantic partition
            // boundary (an admitted offline unit, a synthesized resolved-
            // control-target/fallthrough-continuation unit, a validated
            // candidate root, the IRQ6 handler entry, or a Tier-1 finite
            // indirect-target candidate) even though that target is outside
            // the small bounded diagnostic `candidate_frontier_addresses`
            // set. This is strictly additive: every previously-safe target
            // (a retained entry or a candidate frontier address) remains
            // safe, unchanged.
            if (!retained_entries.contains(edge.target.value) &&
                !represented_nonretained_target(edge.target.value) &&
                // SEG-007-T185 / ADR-0028 §8: shared boundary predicate --
                // identical to build_analysis's straight-line termination
                // check above and C4's per-terminal acceptance.
                !is_semantic_partition_boundary_address(analysis, edge.target.value))
              unsafe_relations.push_back({block.id.entry.value, edge});
          }
          if (!unsafe_relations.empty())
            return record_erase(CompletedPrefixEraseReason::unsafe_outgoing_relation,
                                std::move(unsafe_relations));
          return false;
        });
        changed = prefix.static_blocks.size() != original_count;
      } while (changed);
      // SEG-007-T183 / ADR-0028 §8 instrumentation: the surviving block count
      // and whether the reset/startup entry block itself survived.
      stitch_metrics.retained_block_count_after_pruning =
          static_cast<std::uint32_t>(prefix.static_blocks.size());
      stitch_metrics.ingress_retained =
          prefix.startup_ingress.has_value() &&
          std::any_of(prefix.static_blocks.begin(), prefix.static_blocks.end(), [&](const M68kStaticBlock &block) {
            return same(block.id.entry, prefix.startup_ingress->entry);
          });
      if (unreachable_retained_entries != nullptr)
        *unreachable_retained_entries = currently_unreachable_entries();
    }
    prefix.static_edges.clear();
    std::set<Address> retained_instructions;
    for (const auto &block : prefix.static_blocks)
      for (const auto &instruction : block.instructions)
        retained_instructions.insert(instruction.source.address.value);
    for (const auto instruction_addr : decode_order) {
      if (!retained_instructions.contains(instruction_addr)) continue;
      const auto &decoded = decoded_by_address.at(instruction_addr);
      prefix.decoded.push_back(decoded);
      prefix.ir.push_back(lift_m68k_instruction(decoded));
    }
    // Retain the result of the one discovery-time static-memory resolution.
    // C4 consumes this record; it never re-reads FrontendImage or derives an
    // image offset from a target address while emitting generated C.
    const auto retain_fact = [&](const M68kDecodedInstruction &decoded, const M68kEffectiveAddress &ea,
                                 M68kStaticMemoryFactRole role, M68kMemoryAccessDirection direction) {
      if (!m68k_is_statically_foldable_control_ea(ea)) return;
      const auto address = m68k_canonical_ea_address(ea);
      M68kStaticMemoryFact fact{};
      fact.operation = decoded.provenance;
      fact.role = role;
      fact.address = {TargetAddressSpace::m68k_program, address};
      fact.width = decoded.size;
      fact.direction = direction;
      fact.source_provenance = decoded.provenance;
      if (m68k_startup_ram_range_in_range(address, static_cast<std::uint32_t>(decoded.size))) {
        fact.region = M68kAbsoluteOperandRegion::synthetic_work_ram;
      } else {
        const auto mapped = claims(program.mapping_claims, address);
        if (direction == M68kMemoryAccessDirection::read && mapped.empty()) {
          // A controller-I/O address is never covered by any ROM mapping
          // claim, so this branch never changes behavior for an address that
          // *is* covered by one or more ROM claims -- it only ever applies
          // where the existing single-ROM-claim branch below would already
          // have discarded the fact for lack of any claim at all. Route
          // through the exact same shared gate discovery-time resolution
          // already uses for this purpose
          // (m68k_resolve_absolute_test_operand) rather than re-deriving
          // controller-I/O region membership here.
          const auto routed = m68k_route_genesis_device_access(M68kMemoryAccessRequest{
              M68kProgramAddress{TargetAddressSpace::m68k_program, address}, decoded.size, direction,
              decoded.provenance});
          if (std::holds_alternative<M68kControllerIoResult>(routed))
            fact.region = M68kAbsoluteOperandRegion::controller_io;
          else if (std::holds_alternative<M68kVdpRoutedRead>(routed))
            fact.region = M68kAbsoluteOperandRegion::vdp;
          else if (std::holds_alternative<M68kDeviceRoutedAccess>(routed))
            // SEG-007-T115: a routed READ of a Z80 bus-arbitration register
            // ($A11100 BUSACK poll) or a Z80 program-RAM byte -- deferred to
            // the same shared runtime owner.
            fact.region = M68kAbsoluteOperandRegion::routed_device;
          else
            return;
          prefix.static_memory_facts.push_back(std::move(fact));
          return;
        }
        if (direction == M68kMemoryAccessDirection::write && mapped.empty()) {
          // SEG-007-T113 / SEG-007-T115: write-direction mirror of the read
          // branch above. A direct absolute-operand store whose shape a
          // runtime device owner already supports -- a VDP register-window
          // WORD/LONG store (T113), or a Z80 bus-arbitration / Z80 program-RAM
          // / PSG store (T115) -- is routed through the same shared gate; the
          // runtime is the sole holder of every device's state, so the
          // retained fact carries only the routing decision. Any other
          // unmapped write yields no fact and remains an unmapped-data
          // frontier for the instruction's own block-emission case to
          // classify.
          const auto routed = m68k_route_genesis_device_access(M68kMemoryAccessRequest{
              M68kProgramAddress{TargetAddressSpace::m68k_program, address}, decoded.size, direction,
              decoded.provenance});
          if (std::holds_alternative<M68kVdpRoutedWrite>(routed))
            fact.region = M68kAbsoluteOperandRegion::vdp;
          else if (std::holds_alternative<M68kDeviceRoutedAccess>(routed))
            // SEG-007-T115: a routed STORE into the Z80 bus-arbitration
            // control-register window, the flat Z80 program-RAM window, or the
            // co-located PSG audio port -- same shared runtime owner as the
            // VDP write arm above.
            fact.region = M68kAbsoluteOperandRegion::routed_device;
          else
            return;
          prefix.static_memory_facts.push_back(std::move(fact));
          return;
        }
        if (mapped.size() != 1U || direction == M68kMemoryAccessDirection::write) return;
        const auto &claim = *mapped.front();
        const auto local = static_cast<std::size_t>(claim.image_begin.value + address - claim.target_begin.value);
        const auto width = static_cast<std::size_t>(decoded.size);
        if (local > program.image.bytes.size() || width > program.image.bytes.size() - local) return;
        fact.region = M68kAbsoluteOperandRegion::raw_cartridge_rom;
        for (std::size_t byte = 0; byte < width; ++byte)
          fact.immutable_value = (fact.immutable_value << 8U) | program.image.bytes[local + byte];
      }
      prefix.static_memory_facts.push_back(std::move(fact));
    };
    for (const auto &decoded : prefix.decoded) {
      switch (decoded.kind) {
      case M68kInstructionKind::move:
        retain_fact(decoded, decoded.source_ea, M68kStaticMemoryFactRole::source_read, M68kMemoryAccessDirection::read);
        retain_fact(decoded, decoded.destination_ea, M68kStaticMemoryFactRole::destination_write, M68kMemoryAccessDirection::write);
        break;
      case M68kInstructionKind::movea:
        // MOVEA has a direct An destination, so only its source can be a
        // routed/folded memory operand and only that source owns a fact.
        retain_fact(decoded, decoded.source_ea, M68kStaticMemoryFactRole::source_read, M68kMemoryAccessDirection::read);
        break;
      case M68kInstructionKind::tst:
        retain_fact(decoded, decoded.source_ea, M68kStaticMemoryFactRole::source_read, M68kMemoryAccessDirection::read);
        break;
      case M68kInstructionKind::clr:
      case M68kInstructionKind::set_conditional:  // SEG-021-T016: Scc is CLR-shaped (write-only byte destination)
        retain_fact(decoded, decoded.destination_ea, M68kStaticMemoryFactRole::destination_write, M68kMemoryAccessDirection::write);
        break;
      case M68kInstructionKind::andi:
        // SEG-007-T071: ANDI's destination is read-modify-write, exactly like
        // CLR's destination-only write, so it retains a fact the same way
        // (single destination_write role; ANDI has no separate source
        // operand to retain a fact for -- its source is always immediate).
        retain_fact(decoded, decoded.destination_ea, M68kStaticMemoryFactRole::destination_write, M68kMemoryAccessDirection::write);
        break;
      case M68kInstructionKind::ori:
      case M68kInstructionKind::eori:
        // SEG-007-T167: ORI/EORI mirror ANDI exactly -- an instruction-embedded
        // immediate source (never a memory operand) and a read-modify-write
        // memory destination. Following ANDI's established convention, a single
        // destination_write fact is the retained address/region authority for
        // BOTH routed RMW accesses (the destination read and the destination
        // write); the shared logical_and_immediate C4 emission case threads that
        // one fact's confirmed synthetic_work_ram region into the destination
        // read side as well. See valid_c4_static_memory_fact / the emission-time
        // fact switch, which independently accept only that one role for ori/eori.
        retain_fact(decoded, decoded.destination_ea, M68kStaticMemoryFactRole::destination_write, M68kMemoryAccessDirection::write);
        break;
      case M68kInstructionKind::logical_and:
      case M68kInstructionKind::logical_or:
      case M68kInstructionKind::eor:
        // SEG-007-T167: non-immediate AND/OR/EOR. Exactly one operand is a data
        // register; the other may be a foldable-control memory EA. Mirror ADD's
        // source-read plus destination read+write RMW fact shape verbatim (EOR
        // has only the `Dn,<ea>` form; AND/OR also have `<ea>,Dn`). A
        // register/auto-updating/register-indirect operand yields no fact
        // (retain_fact is a no-op for those modes).
        retain_fact(decoded, decoded.source_ea, M68kStaticMemoryFactRole::source_read, M68kMemoryAccessDirection::read);
        if (decoded.destination_ea.mode != M68kEaMode::data_register &&
            decoded.destination_ea.mode != M68kEaMode::address_register) {
          retain_fact(decoded, decoded.destination_ea, M68kStaticMemoryFactRole::destination_read,
                      M68kMemoryAccessDirection::read);
          retain_fact(decoded, decoded.destination_ea, M68kStaticMemoryFactRole::destination_write,
                      M68kMemoryAccessDirection::write);
        }
        break;
      case M68kInstructionKind::add:
        // ADD's source is read first. A memory destination is then an RMW
        // operand, so retain its read and write facts independently; C4 uses
        // these existing roles to choose the one representable routed/folded
        // operand without re-resolving an address from the image.
        retain_fact(decoded, decoded.source_ea, M68kStaticMemoryFactRole::source_read, M68kMemoryAccessDirection::read);
        if (decoded.destination_ea.mode != M68kEaMode::data_register &&
            decoded.destination_ea.mode != M68kEaMode::address_register) {
          retain_fact(decoded, decoded.destination_ea, M68kStaticMemoryFactRole::destination_read,
                      M68kMemoryAccessDirection::read);
          retain_fact(decoded, decoded.destination_ea, M68kStaticMemoryFactRole::destination_write,
                      M68kMemoryAccessDirection::write);
        }
        break;
      case M68kInstructionKind::sub:
        // SEG-007-T170: SUB shares ADD's exact two-operand shape (`<ea>,Dn`
        // or `Dn,<ea>`) -- its source is read first, and a memory
        // destination is then an RMW operand retaining independent read and
        // write facts, mirroring ADD's own fact shape verbatim.
        retain_fact(decoded, decoded.source_ea, M68kStaticMemoryFactRole::source_read, M68kMemoryAccessDirection::read);
        if (decoded.destination_ea.mode != M68kEaMode::data_register &&
            decoded.destination_ea.mode != M68kEaMode::address_register) {
          retain_fact(decoded, decoded.destination_ea, M68kStaticMemoryFactRole::destination_read,
                      M68kMemoryAccessDirection::read);
          retain_fact(decoded, decoded.destination_ea, M68kStaticMemoryFactRole::destination_write,
                      M68kMemoryAccessDirection::write);
        }
        break;
      case M68kInstructionKind::subi:
        // SEG-007-T170: SUBI's source is always the instruction-embedded
        // immediate (never a memory operand), so only its destination can
        // retain a fact -- a full RMW operand exactly like SUBQ's own
        // destination shape immediately below.
        if (decoded.destination_ea.mode != M68kEaMode::data_register &&
            decoded.destination_ea.mode != M68kEaMode::address_register) {
          retain_fact(decoded, decoded.destination_ea, M68kStaticMemoryFactRole::destination_read,
                      M68kMemoryAccessDirection::read);
          retain_fact(decoded, decoded.destination_ea, M68kStaticMemoryFactRole::destination_write,
                      M68kMemoryAccessDirection::write);
        }
        break;
      case M68kInstructionKind::cmp:
        // SEG-007-T216: plain `CMP <ea>,Dn` shares CMPA's exact source-read
        // shape below -- its destination is always a data register (never a
        // memory operand needing a fact), and its source is read once to
        // form the CCR-only comparison result, exactly mirroring
        // classify_m68k_c4_gap_shapes's own `compare`/`compare_address`
        // combined branch in libs/codegen/c11/src/frontend.cpp. This switch
        // previously had no case for plain `cmp` at all (only its `cmpa`/
        // `cmpi` siblings immediately below/above), so a real foldable-EA
        // CMP instruction never retained any fact, unconditionally producing
        // a `GENESIS_C4_LOWERING_DIMENSIONS_CMP_MISSING_FACT` C4 build-time
        // stop the instant one became reachable -- the identical missing-
        // case shape SEG-007-T170's SUBI fix and this switch's own `cmpi`
        // case comment (immediately below) already document for their own
        // siblings.
        retain_fact(decoded, decoded.source_ea, M68kStaticMemoryFactRole::source_read,
                    M68kMemoryAccessDirection::read);
        break;
      case M68kInstructionKind::cmpa:
      case M68kInstructionKind::adda:
      case M68kInstructionKind::suba:
        // The fixed An destination has no memory fact; retain the source read
        // through the same C4 fact/routing seam as ADD's source.
        retain_fact(decoded, decoded.source_ea, M68kStaticMemoryFactRole::source_read,
                    M68kMemoryAccessDirection::read);
        break;
      case M68kInstructionKind::subq:
      case M68kInstructionKind::addq:
      case M68kInstructionKind::addi:
        // SEG-007-T165: SUBQ's source is always the quick immediate (never a
        // memory operand), so only its destination can retain a fact. Its
        // destination is a full RMW operand exactly like ADD's -- retain
        // both the read and write facts independently for a foldable
        // absolute destination; a register-indirect/predecrement/
        // postincrement/indexed/displacement destination is handled entirely
        // through C4's existing dynamic runtime-routed read/write path and
        // never needs a fact at all (retain_fact is a no-op for those modes).
        // SEG-007-T174 follow-up fix (reviewer-directed, composed-hints
        // regression): ADDQ shares this identical shape byte-for-byte (its
        // own source is also always the quick immediate, per the M68000 ISA
        // -- `ADDQ #<data>,<ea>`) but this switch previously had no case for
        // it at all, so a real, foldable-absolute-EA ADDQ instruction (e.g.
        // `ADDQ.L #1,(abs).w`, the classic RAM-counter-increment idiom) that
        // only became reachable once a real, substantially wider validated
        // Ghidra-assisted candidate-root set restored the already-solved
        // `VBlank_Index` Tier-1 dispatch and let generated execution proceed
        // into one of its real handler bodies for the first time silently
        // fell through to `default:`/no fact at all, producing a
        // `GENESIS_C4_LOWERING_DIMENSIONS_ADD_QUICK_MISSING_FACT` C4
        // build-time stop for an instruction shape this switch was always
        // meant to cover, mirroring SUBQ's own established case verbatim.
        if (decoded.destination_ea.mode != M68kEaMode::data_register &&
            decoded.destination_ea.mode != M68kEaMode::address_register) {
          retain_fact(decoded, decoded.destination_ea, M68kStaticMemoryFactRole::destination_read,
                      M68kMemoryAccessDirection::read);
          retain_fact(decoded, decoded.destination_ea, M68kStaticMemoryFactRole::destination_write,
                      M68kMemoryAccessDirection::write);
        }
        break;
      case M68kInstructionKind::shift_rotate:
        // SEG-021-T009: a memory-word shift/rotate is a one-address RMW operand exactly like NOT (retaining
        // both facts for a foldable absolute destination); the register form has no memory operand.
        if (decoded.destination_ea.mode != M68kEaMode::data_register) {
          retain_fact(decoded, decoded.destination_ea, M68kStaticMemoryFactRole::destination_read,
                      M68kMemoryAccessDirection::read);
          retain_fact(decoded, decoded.destination_ea, M68kStaticMemoryFactRole::destination_write,
                      M68kMemoryAccessDirection::write);
        }
        break;
      // SEG-021-T014: NEG/NEGX are one-address RMW operands with NOT's exact fact shape.
      case M68kInstructionKind::negate_word:
      case M68kInstructionKind::negate_extended:
      case M68kInstructionKind::negate_decimal:
      case M68kInstructionKind::test_and_set:  // SEG-021-T016: TAS is a byte one-address RMW like NOT
      case M68kInstructionKind::not_operand:
        // SEG-007-T168: NOT has no second operand at all (unlike SUBQ's
        // quick-immediate source, and unlike ANDI/ORI/EORI, which do carry
        // one, just never a memory one) -- its sole destination is a full
        // RMW operand, retaining both the read and write facts independently
        // exactly like SUBQ's own destination shape immediately above. A
        // register-indirect/predecrement/postincrement/indexed/displacement
        // destination is handled entirely through C4's existing dynamic
        // runtime-routed read/write path and never needs a fact at all
        // (retain_fact is a no-op for those modes).
        if (decoded.destination_ea.mode != M68kEaMode::data_register &&
            decoded.destination_ea.mode != M68kEaMode::address_register) {
          retain_fact(decoded, decoded.destination_ea, M68kStaticMemoryFactRole::destination_read,
                      M68kMemoryAccessDirection::read);
          retain_fact(decoded, decoded.destination_ea, M68kStaticMemoryFactRole::destination_write,
                      M68kMemoryAccessDirection::write);
        }
        break;
      case M68kInstructionKind::btst:
        // BTST is read-only: its bit-number source is Dn/immediate and the
        // destination is the sole potentially memory-referencing operand.
        retain_fact(decoded, decoded.destination_ea, M68kStaticMemoryFactRole::destination_read,
                    M68kMemoryAccessDirection::read);
        break;
      case M68kInstructionKind::bchg:
      case M68kInstructionKind::bclr:
      case M68kInstructionKind::bset:
        // SEG-007-T209: unlike BTST (read-only), BCHG/BCLR/BSET always
        // read-modify-write their destination -- the identical SUBQ/NOT
        // destination-only-with-implicit-read RMW shape above, retaining
        // both the read and write facts independently for a foldable
        // absolute destination. A register-indirect/predecrement/
        // postincrement/indexed/displacement destination needs no fact at
        // all (retain_fact is a no-op for those modes; C4's existing
        // dynamic runtime-routed read/write path handles them).
        retain_fact(decoded, decoded.destination_ea, M68kStaticMemoryFactRole::destination_read,
                    M68kMemoryAccessDirection::read);
        retain_fact(decoded, decoded.destination_ea, M68kStaticMemoryFactRole::destination_write,
                    M68kMemoryAccessDirection::write);
        break;
      case M68kInstructionKind::cmpi:
        // SEG-007-T174 follow-up fix (reviewer-directed, composed-hints
        // regression): CMPI's immediate source is never a memory fact; its
        // destination is read-only (never written), forming the CCR-only
        // comparison result -- the identical destination_read-only shape
        // BTST's own case immediately above already uses. This switch
        // previously had no case for cmpi at all, so a real foldable-EA
        // CMPI instruction never retained any fact, unconditionally
        // producing a `GENESIS_C4_LOWERING_DIMENSIONS_CMPI_MISSING_FACT` C4
        // build-time stop the instant one became reachable.
        retain_fact(decoded, decoded.destination_ea, M68kStaticMemoryFactRole::destination_read,
                    M68kMemoryAccessDirection::read);
        break;
      default: break;
      }
    }
    // SEG-007-T075: the bounded adjacent LEA->MOVEM producer/consumer fact
    // pass (docs/architecture/c4-movem-adjacent-lea-constant-propagation-
    // contract.md, conditions 1-6). Wholly additive and separate from
    // retain_fact above -- it never changes retain_fact's own per-instruction
    // fact shape or call sites, and it never records a fact for any producer
    // other than load_effective_address or any consumer other than a
    // memory_to_registers movem_transfer.
    {
      std::map<Address, const M68kDecodedInstruction *> decoded_by_address;
      for (const auto &decoded : prefix.decoded)
        decoded_by_address.emplace(decoded.provenance.source.address.value, &decoded);
      for (const auto &block : prefix.static_blocks) {
        for (std::size_t index = 1; index < block.instructions.size(); ++index) {
          const auto producer_found = decoded_by_address.find(block.instructions[index - 1].source.address.value);
          const auto consumer_found = decoded_by_address.find(block.instructions[index].source.address.value);
          if (producer_found == decoded_by_address.end() || consumer_found == decoded_by_address.end()) continue;
          const auto &producer = *producer_found->second;
          const auto &consumer = *consumer_found->second;
          // Condition 3: producer is a foldable-source LEA.
          if (producer.kind != M68kInstructionKind::lea ||
              !m68k_is_statically_foldable_control_ea(producer.source_ea))
            continue;
          // Condition 4: consumer is a memory_to_registers MOVEM whose
          // register-indirect/(An)+-family source names the exact same
          // address register the producer just set.
          if (consumer.kind != M68kInstructionKind::movem ||
              consumer.movem_direction != M68kMovemDirection::memory_to_registers ||
              (consumer.source_ea.mode != M68kEaMode::address_indirect &&
               consumer.source_ea.mode != M68kEaMode::address_postinc) ||
              consumer.source_ea.reg != producer.destination_ea.reg)
            continue;
          // SEG-007-T075 fix: this must be the EXACT raw literal LEA's own
          // C4 emission (case M68kIrKind::load_effective_address, the
          // absolute_word/absolute_long/pc_disp16 branch) writes into the
          // address register -- producer.source_ea.absolute_address itself,
          // never m68k_canonical_ea_address's masked/canonicalized
          // value. For absolute_long/pc_disp16 the two already agree, but for
          // absolute_word the canonical helper masks the decode-time
          // sign-extended field to 24 bits while LEA's own statement writes
          // the raw sign-extended 32-bit value unmasked; using the masked
          // value here would silently fold a different address than LEA
          // actually computes at runtime whenever bit 15 of the 16-bit
          // absolute_word field is set. Using the raw value instead keeps
          // this the single address-resolution formula: any producer whose
          // raw value falls outside a real mapping claim (exactly what
          // happens for a bit-15-set absolute_word case) simply fails the
          // claims() containment check below and never folds, matching the
          // routed path's own fail-closed behavior for that same address.
          const auto base = producer.source_ea.absolute_address;
          // Register-indirect/(An)+ MOVEM memory_to_registers is never the
          // reversed predecrement order (decode's own legal-EA set never
          // selects predecrement for that direction).
          const auto order =
              m68k_movem_transfer_order(consumer.movem_register_mask, M68kMovemTransferOrder::ascending);
          const auto width = static_cast<std::uint32_t>(consumer.size);
          // Condition 6: the whole transfer span resolves inside exactly one
          // mapping claim, never merely the base address.
          const auto mapped = claims(program.mapping_claims, base);
          if (mapped.size() != 1U) continue;
          const auto &claim = *mapped.front();
          const auto span = static_cast<std::uint64_t>(order.size()) * width;
          if (static_cast<std::uint64_t>(base) + span > claim.target_end.value) continue;
          std::vector<std::uint32_t> values;
          values.reserve(order.size());
          bool bounds_ok = true;
          for (std::size_t slot = 0; slot < order.size() && bounds_ok; ++slot) {
            const auto address = base + static_cast<std::uint32_t>(slot) * width;
            const auto local =
                static_cast<std::size_t>(claim.image_begin.value + address - claim.target_begin.value);
            if (local > program.image.bytes.size() || width > program.image.bytes.size() - local) {
              bounds_ok = false;
              break;
            }
            std::uint32_t value = 0U;
            for (std::uint32_t byte = 0; byte < width; ++byte)
              value = (value << 8U) | program.image.bytes[local + byte];
            values.push_back(value);
          }
          if (!bounds_ok) continue;
          M68kMovemAdjacentLeaFact fact{};
          fact.producer = producer.provenance;
          fact.consumer = consumer.provenance;
          fact.address_register = producer.destination_ea.reg;
          fact.resolved_base = base;
          fact.transfer_values = std::move(values);
          prefix.movem_adjacent_lea_facts.push_back(std::move(fact));
        }
      }
    }
    // SEG-007-T077: the generic immutable/generated cartridge-data region
    // ownership fact pass -- one fact per mapping claim eligible to back a
    // runtime-computed (non-constant-foldable) effective address through
    // genesis_route_access, instead of a fourth one-instance scalar fold.
    // See docs/decisions/0006-generic-cartridge-data-region-ownership.md.
    // Region mapping/bounds reuse `valid_claim`, the exact same invariant
    // genesis_valid_provenance already checks for every other mapping claim;
    // ownership/immutability is implicit (every claim reaching this pass is
    // `raw_cartridge_rom`-backed by construction at every production call
    // site); the hardware-ROM-window bound
    // (target_end <= 0x00400000) matches genesis_route_access's own existing
    // ROM branch exactly, never overlapping the work-RAM or device windows.
    // Wholly additive: it never changes retain_fact's or the movem-adjacent-
    // LEA pass's own shapes or call sites, and it never changes which EA a
    // C4 switch case folds versus routes -- see the switch cases below,
    // unchanged by this pass.
    for (const auto &claim : program.mapping_claims) {
      if (!valid_claim(claim, program.image.bytes.size()) ||
          claim.target_end.value > UINT32_C(0x00400000))
        continue;
      M68kOwnedCartridgeRegionFact fact{};
      fact.claim = claim;
      fact.resolved_bytes.assign(
          program.image.bytes.begin() + static_cast<std::ptrdiff_t>(claim.image_begin.value),
          program.image.bytes.begin() + static_cast<std::ptrdiff_t>(claim.image_end.value));
      prefix.owned_cartridge_region_facts.push_back(std::move(fact));
    }
    const auto source_retained = [&](const InstructionProvenance &provenance) {
      return retained_instructions.contains(provenance.source.address.value);
    };
    const auto retained_entry_addr = [&](Address value) {
      return std::any_of(prefix.static_blocks.begin(), prefix.static_blocks.end(),
                         [&](const M68kStaticBlock &block) { return block.id.entry.value == value; });
    };
    // SEG-007-T183 / ADR-0028 §8: at large multi-unit scale, a synthesized
    // fallthrough-continuation root (ADR-0027) can coincide with an address
    // that is ALSO the plain fallthrough/taken successor of an ordinary
    // branch instruction, or the plain block-entry-merge fallthrough edge
    // `build_analysis`'s own per-entry reconstruction already synthesizes
    // (`block_edges`) -- either way, both the ordinary edge and the
    // synthesized `fallthrough_continuation` edge then get merged for the
    // exact SAME (source, target) transition. A branch/return/indirect
    // terminal's own edges (or an already-covered plain merge fallthrough)
    // already fully describe that control flow (this is the exact shape the
    // C4 completeness check below already documents as invalid -- "a
    // fallthrough_continuation edge is valid only on a non-terminal or call
    // terminal"), so a redundant synthesized copy sharing the identical
    // (source, target) pair is dropped here as a pure dedup, never a new
    // admission rule or a change to which addresses are ever retained.
    using EdgePair = std::pair<Address, Address>;
    std::set<EdgePair> covered_by_other_edge_kind;
    for (const auto &edge : discovered_edges)
      if (edge.kind == M68kStaticEdgeKind::fallthrough || edge.kind == M68kStaticEdgeKind::direct_branch ||
          edge.kind == M68kStaticEdgeKind::return_to_continuation ||
          edge.kind == M68kStaticEdgeKind::indirect_branch)
        covered_by_other_edge_kind.emplace(edge.source_instruction.source.address.value, edge.target.value);
    for (const auto &edge : block_edges)
      if (edge.kind == M68kStaticEdgeKind::fallthrough)
        covered_by_other_edge_kind.emplace(edge.source_instruction.source.address.value, edge.target.value);
    for (const auto &edge : discovered_edges) {
      if (!source_retained(edge.source_instruction)) continue;
      // SEG-007-T181 / ADR-0027: a synthesized fallthrough-continuation edge is
      // meaningful only when its target block is still a retained entry in the
      // pruned prefix; if that continuation unit was pruned (downstream of the
      // frontier or not entry-connected), drop the dangling adjacency edge
      // exactly as build_analysis already drops a dangling plain `fallthrough`.
      if (edge.kind == M68kStaticEdgeKind::fallthrough_continuation && !retained_entry_addr(edge.target.value))
        continue;
      if (edge.kind == M68kStaticEdgeKind::fallthrough_continuation &&
          covered_by_other_edge_kind.contains(
              {edge.source_instruction.source.address.value, edge.target.value}))
        continue;
      // SEG-007-T183 / ADR-0028 §8: a `return_to_continuation` edge is
      // sourced at the callee's own RTS, which can remain retained
      // independent of whether the call's own CALLER instruction is
      // retained -- at large multi-unit scale a callee unit can complete
      // and stay retained while the specific caller that opened this exact
      // call identity belongs to a separately unretained unit (its own
      // `direct_call`/`indirect_call` edge and `M68kStaticFrame` are already
      // dropped by this exact same `source_retained` filter, applied to the
      // caller, a few lines below). Retaining this orphaned return edge
      // anyway would assert a call frame that no longer exists in the
      // retained program, so `runtime_frontier_eligible`'s own `has_frame`
      // check would then legitimately reject the whole promotion over a call
      // identity that was never actually admitted. Drop it here instead,
      // mirroring the `fallthrough_continuation` guard immediately above --
      // this is strictly a dangling-edge cleanup (no new admission rule, no
      // new edge kind, no change to which addresses are ever retained).
      if (edge.kind == M68kStaticEdgeKind::return_to_continuation && edge.call &&
          !source_retained(edge.call->caller))
        continue;
      // SEG-007-T185 / ADR-0028 §8.2 (direct-control target preservation): also
      // drop a `return_to_continuation` edge whose CONTINUATION block was pruned
      // (its target is no longer a retained entry), mirroring the
      // `fallthrough_continuation` retained-target guard above exactly. Before
      // this, such a dangling return edge survived into `prefix.static_edges` and
      // tripped `runtime_frontier_eligible`'s hard per-edge target invariant
      // (every edge target must be a retained entry / represented frontier /
      // semantic-partition boundary), which forced the completed-prefix erase
      // pass to instead drop the shared callee block -- cascading a single
      // pruned continuation into a program-wide collapse of that call tree and
      // leaving generated direct `BSR`/`JSR` sites transferring to a callee with
      // no emitted block. Dropping only the dead return sub-path keeps the
      // callee (and every other caller) intact. Strictly a dangling-edge
      // cleanup: no new admission rule, edge kind, or retained address. Gated to
      // the multi-unit offline route so the raw single-unit route stays
      // byte-identical.
      if (edge.kind == M68kStaticEdgeKind::return_to_continuation &&
          !analysis.semantic_partition_boundary_addresses.empty() &&
          !retained_entry_addr(edge.target.value) &&
          !represented_nonretained_target(edge.target.value) &&
          !is_semantic_partition_boundary_address(analysis, edge.target.value))
        continue;
      prefix.static_edges.push_back(edge);
    }
    for (const auto &edge : block_edges)
      if (source_retained(edge.source_instruction)) prefix.static_edges.push_back(edge);
    std::erase_if(prefix.static_frames, [&](const M68kStaticFrame &frame) {
      return !source_retained(frame.call.caller);
    });
    // SEG-007-T124 / ADR-0009: an indirect target-EA-set fact is retained
    // exactly when its own source instruction is retained, mirroring the
    // edges/frames filters directly above.
    prefix.indirect_target_ea_sets.clear();
    for (const auto &set : discovered_indirect_target_ea_sets)
      if (source_retained(set.source_instruction)) prefix.indirect_target_ea_sets.push_back(set);
    // SEG-007-T174 / ADR-0024: unlike `indirect_target_ea_sets` above, this
    // weaker Tier-2-eligible fact is deliberately left as `prefix` already
    // carries it (an unfiltered copy of `analysis.unproven_indirect_control_
    // ea_sets`), never cleared/re-filtered by `source_retained` here. Its
    // whole reason to exist is to describe exactly the FAILING frontier
    // instruction itself -- which, by construction,
    // `exclude_frontier_instruction` above always excludes from ever
    // becoming part of a retained static block, so it would never satisfy
    // `source_retained` at all. It is instead consumed at C4 emission time by
    // an exact source-instruction-address match against one specific
    // represented frontier's own diagnostic provenance (see
    // `build_genesis_frontier_stop_function`); an entry that never matches
    // any represented frontier is simply never consulted and never causes any
    // C4 rejection.
    // SEG-007-T252 / ADR-0040 correction: the former ADR-0016 static finite-
    // loop-progress proof and ADR-0017/0018/0019 generated data-transform
    // progress proof were computed here. Their only consumer was the
    // generated-runtime progress-watchdog note-emission machinery ADR-0040
    // retired; this task removes the now-dead producers themselves (see
    // ADR-0040 §7 and `libs/cpu/m68k/src/static_loop_proof.cpp`'s removal).
    return prefix;
  };
  // SEG-007-T064 / ADR-0021 §4: shared classification, reused for the
  // primary failure and every best-effort secondary below -- the same
  // free-function `classify_frontier` above, hoisted out of this function so
  // ADR-0021 §4's IRQ6-root fatal-probe-failure discriminator can reuse it
  // directly, before this aggregation runs, with no second mechanism.
  // SEG-007-T064: the tuple sort/dedup key required by the amended
  // architecture contract's §1.3 ordering rule -- (source address, target
  // address or zero, class ordinal).
  const auto frontier_sort_key = [](const UnresolvedFrontier &value) -> std::tuple<Address, Address, int> {
    const Address source = value.diagnostic.provenance ? value.diagnostic.provenance->source.address.value : 0U;
    Address target = 0U;
    if ((value.class_ == GenesisFrontierClass::unsupported_device_access ||
         value.class_ == GenesisFrontierClass::unsupported_memory_region) && value.access)
      target = value.access->address.value;
    return {source, target, static_cast<int>(value.class_)};
  };
  const auto partial_or_rejection = [&]() -> FrontendResult {
    // SEG-007-T032: this is the exact `frontier_access` discard site
    // SEG-007-T031 identified. Populate the sanitized, non-address shape
    // projection only for the scoped case (general_startup profile,
    // unsupported_device_region_controller_io category, an actually
    // computed frontier_access) before returning the plain rejection; every
    // other category/profile/unpopulated case leaves this field untouched
    // (std::nullopt), and a promoted FrontendPartialProgram never reaches
    // this helper at all.
    const auto reject = [&]() -> FrontendResult {
      if (failure->profile == M68kFrontendProfile::general_startup &&
          failure->category == DirectFlowDiagnostic::unsupported_device_region_controller_io &&
          frontier_access)
        failure->controller_io_access_shape = m68k_classify_controller_io_access_shape(*frontier_access);
      return *failure;
    };
    const auto frontier_class = classify_frontier(*failure, frontier_access.has_value());
    CompletedPrefixEraseTransaction adr0038_erase_transaction;
    auto prefix = build_analysis(true, true, &adr0038_erase_transaction);
    // A Tier-1 relation is an all-members relation.  Before C4 decides whether
    // an unrepresentable member requires its source stop, give every already
    // decoded candidate the same ordinary leader/block reconstruction that an
    // existing direct-control destination receives.  This is deliberately not
    // an admission path: it creates no decode, edge, frame, root, or candidate
    // fact.  It only lets `build_analysis` split and re-prune the already
    // validated aggregate at a candidate which was decoded in another unit but
    // was not previously an emitted block entry.  A candidate that remains
    // incomplete or is pruned by the existing invariants is left unresolved for
    // the existing fail-closed C4 ownership check; boundary membership alone
    // never makes it executable.
    if (prefix) {
      std::set<Address> retained_entries;
      std::set<Address> retained_sources;
      for (const auto &block : prefix->static_blocks) {
        retained_entries.insert(block.id.entry.value);
        for (const auto &instruction : block.instructions)
          retained_sources.insert(instruction.source.address.value);
      }
      bool rebuilt = false;
      for (const auto &relation : prefix->indirect_target_ea_sets) {
        if (!retained_sources.contains(relation.source_instruction.source.address.value)) continue;
        for (const auto &candidate : relation.candidates) {
          if (candidate.space != TargetAddressSpace::m68k_program || retained_entries.contains(candidate.value) ||
              !merged_decoded.contains(candidate.value))
            continue;
          if (block_entries.insert(candidate.value).second) {
            block_entry_order.push_back(candidate.value);
            rebuilt = true;
          }
        }
      }
      if (rebuilt) prefix = build_analysis(true, true, &adr0038_erase_transaction);
    }
    // SEG-007-T214 / ADR-0028 §9: authoritative exact direct-control target
    // closure -- PHASE A (obligation extraction, §9.2 promotion / fresh
    // materialization, and the single rebuild-and-reprune cycle). Unconditional
    // -- NOT gated on `analysis.semantic_partition_boundary_addresses` being
    // non-empty (§9.7): on the raw/no-inventory route the obligation set below
    // is naturally already fully satisfied (the entry-rooted walk already
    // retains every direct-control target it reaches), so this performs zero
    // materializations as a measured consequence, never a coded precondition.
    //
    // PHASE B (the final per-target satisfied / typed-frontier / fail-closed
    // resolution, §9.1/§9.6/§9.4) is deliberately deferred to AFTER the
    // ordinary primary/secondary discovery-issue frontier construction below:
    // an obligated target that a ceiling trip or an ordinary open-control-edge
    // rejection ALREADY represents through that pre-existing, independent
    // mechanism (e.g. `discovery_prefix_boundary`/`known_but_unemitted_target`
    // built directly from the walker's own `M68kDiscoveryIssue`, not from a
    // decoded instruction) must count as satisfied -- it is never limited to
    // representations this closure itself constructs from `merged_decoded`.
    std::set<Address> authoritative_unsatisfied_targets;
    // Populated in Phase B below with the address of every obligated target
    // this closure represents via its own newly-constructed typed frontier
    // (never via ordinary discovery-issue frontier construction). Folded into
    // the SEG-007-T064 sibling/retained-address consistency check below so a
    // closure-represented target is recognized as an intentional retained
    // frontier, not an inconsistency.
    std::set<Address> authoritative_exact_closure_represented_addresses;
    if (prefix) {
      // §9.1: the obligation set is extracted from the INITIAL completed-
      // prefix erase-pass fixpoint's own stable output -- `prefix`, produced
      // by the single `build_analysis(true, true)` call directly above,
      // unchanged from today's pre-existing behavior. Obligated edge kinds:
      // `direct_branch` (also covers a folded unconditional direct JMP, per
      // §8.1) and `direct_call` (BSR / folded direct JSR) -- the same exact
      // direct-control edge family §8.2's `direct_control_target_entries`
      // already collects.
      std::set<Address> retained_entries_initial;
      for (const auto &block : prefix->static_blocks) retained_entries_initial.insert(block.id.entry.value);
      std::set<Address> authoritative_obligations;
      for (const auto &edge : prefix->static_edges) {
        if (edge.kind != M68kStaticEdgeKind::direct_branch && edge.kind != M68kStaticEdgeKind::direct_call) continue;
        if (edge.target.space != TargetAddressSpace::m68k_program) continue;
        authoritative_obligations.insert(edge.target.value);
      }
      stitch_metrics.authoritative_exact_target_count =
          static_cast<std::uint32_t>(authoritative_obligations.size());
      std::set<Address> unsatisfied_targets;
      for (const auto target : authoritative_obligations) {
        if (retained_entries_initial.contains(target))
          ++stitch_metrics.authoritative_exact_targets_already_represented;
        else
          unsatisfied_targets.insert(target);
      }
      // §9.2/§9.1: process every unsatisfied target at most once (§9.3 --
      // process-once, no retry loop). A promotion (already decoded,
      // non-entry) is a pure leader-set membership change: no new
      // `M68kStaticGraphWalker` root, no synthesized-unit-ceiling
      // consumption. A target with no decoded facts at all attempts a fresh
      // materialization reusing the exact same mechanism §1's
      // resolved-control-target synthesis and the late-indirect closure
      // above already use -- this DOES consume the shared
      // `m68k_fallthrough_continuation_unit_ceiling` (§9.3's ceiling scope
      // correction). A target this loop cannot even attempt to materialize
      // (the shared ceiling is already exhausted) is left unsatisfied and
      // falls through to the typed-frontier / fail-closed resolution below,
      // exactly like a target whose fresh walk failed validation.
      bool leader_set_or_unit_set_augmented = false;
      std::uint32_t authoritative_closure_root_counter = 1U;
      // §9.3: a monotonically growing PROCESSED-target set, seeded from every
      // obligated target present at the erase pass's fixpoint. Promoting or
      // materializing a target can expose NEW exact direct-control edges from
      // its own newly-merged instructions; only newly-discovered, not-yet
      // -processed targets are appended to the worklist (identical
      // accumulation pattern to §1/§3's `synthesized_control_roots` fixed
      // point). Each candidate target is closure-processed at most once.
      std::set<Address> authoritative_closure_processed;
      std::vector<Address> authoritative_closure_worklist(unsatisfied_targets.begin(), unsatisfied_targets.end());
      for (std::size_t worklist_index = 0; worklist_index < authoritative_closure_worklist.size();
           ++worklist_index) {
        const auto target = authoritative_closure_worklist[worklist_index];
        if (!authoritative_closure_processed.insert(target).second) continue;
        const auto before_edge_count = merged_edges.size();
        if (merged_decoded.contains(target)) {
          // §9.2 promotion: already decoded, non-entry. Splits the
          // containing unit's block at `target` the next time
          // `build_analysis` reconstructs blocks (identical splitting
          // mechanism to §7); adds no new decode, no new edges, and consumes
          // no synthesized-unit-ceiling budget.
          if (block_entries.insert(target).second) {
            block_entry_order.push_back(target);
            leader_set_or_unit_set_augmented = true;
          }
          continue;
        }
        // A target that is already the subject of the aggregate's own
        // primary/secondary discovery issue (e.g. an ordinary ceiling-trip
        // `discovery_prefix_boundary` candidate the pre-existing frontier
        // mechanism already classifies below) has its own documented
        // supported-dispatch representation path (§9.1's third satisfying
        // case) and is deliberately never eagerly re-walked/materialized here
        // -- that would bypass the existing, independently-tested
        // candidate-frontier / `runtime_confirmed_seeds` supersession
        // machinery this closure must not duplicate or short-circuit. Left
        // unsatisfied for Phase B, which recognizes it once the ordinary
        // primary/secondary frontiers are built.
        const bool already_a_discovery_issue_address =
            (failure->source_address && failure->source_address->value == target) ||
            std::any_of(secondary_failures.begin(), secondary_failures.end(), [&](const auto &secondary) {
              return secondary.first.source_address && secondary.first.source_address->value == target;
            });
        if (already_a_discovery_issue_address) continue;
        if (continuation_roots.size() + synthesized_control_roots.size() >=
            m68k_fallthrough_continuation_unit_ceiling)
          continue;  // shared ceiling exhausted; resolved by the fail-closed check below.
        std::set<Address> boundary = admitted_units;
        boundary.insert(synthesized_control_roots.begin(), synthesized_control_roots.end());
        boundary.insert(entry.value);
        boundary.erase(target);
        std::set<Address> cont_boundary = continuation_roots;
        cont_boundary.erase(target);
        const M68kProgramAddress target_address{TargetAddressSpace::m68k_program, target};
        const auto probe = discover_m68k_static_graph(target_address, limits, environment, boundary, cont_boundary);
        const bool entry_decoded = probe.decode_cache.find(target_address) != nullptr;
        const bool fatal_probe =
            (probe.primary_issue && is_fatal_probe_failure(*probe.primary_issue)) ||
            std::any_of(probe.secondary_issues.begin(), probe.secondary_issues.end(),
                        [&](const M68kDiscoveryIssue &issue) { return is_fatal_probe_failure(issue); });
        const bool truncated = candidate_walk_truncated(probe);
        if (!entry_decoded || fatal_probe || truncated) continue;  // validation failure; resolved below.
        merge_root_result({StaticProgramRootKind::asynchronous_hardware, authoritative_closure_root_counter++},
                           probe);
        if (aggregation_conflict) break;
        if (synthesized_control_roots.insert(target).second) {
          analysis.semantic_partition_boundary_addresses.insert(target);
          ++stitch_metrics.synthesized_resolved_control_target_unit_count;
        }
        leader_set_or_unit_set_augmented = true;
        // §9.3: a freshly-materialized unit's own instructions can carry NEW
        // exact direct-control edges (recursive closure -- S -> T -> U).
        // Every such newly-merged edge's target that is not yet a
        // closure-processed obligation is appended to this SAME worklist, so
        // it is closure-processed within this same pre-rebuild pass (not a
        // second rebuild-and-reprune cycle -- §9.3/Non-goals).
        for (std::size_t edge_index = before_edge_count; edge_index < merged_edges.size(); ++edge_index) {
          const auto &new_edge = merged_edges[edge_index];
          if (new_edge.kind != M68kStaticEdgeKind::direct_branch && new_edge.kind != M68kStaticEdgeKind::direct_call)
            continue;
          if (new_edge.target.space != TargetAddressSpace::m68k_program) continue;
          if (authoritative_closure_processed.contains(new_edge.target.value)) continue;
          authoritative_closure_worklist.push_back(new_edge.target.value);
        }
      }
      if (aggregation_conflict) {
        auto r = rejected(DirectFlowDiagnostic::startup_graph_mismatch, program);
        set_source(r, entry);
        return r;
      }
      // §9.1's single rebuild-and-reprune cycle: `build_analysis` is
      // reconstructed ONCE from the augmented leader set / merged units, and
      // the completed-prefix erase-pass fixpoint is run ONCE MORE over the
      // rebuilt block set. Never repeated: no second rebuild cycle, no retry
      // loop (§9.3).
      if (leader_set_or_unit_set_augmented) {
        analysis.static_edges = merged_edges;
        analysis.static_frames = merged_frames;
        stitch_metrics.authoritative_exact_closure_rounds = 1U;
        prefix = build_analysis(true, true);
      }
      if (!prefix) {
        auto r = rejected(DirectFlowDiagnostic::startup_graph_mismatch, program);
        set_source(r, entry);
        return r;
      }
      std::set<Address> retained_entries_final;
      for (const auto &block : prefix->static_blocks) retained_entries_final.insert(block.id.entry.value);
      // §9.3: the FINAL obligation count is the original erase-pass-fixpoint
      // obligation set (`authoritative_obligations`, already recorded above,
      // including every target already represented before closure ran) plus
      // every recursively-discovered obligation (S -> T -> U) appended to the
      // worklist that was NOT already a member of that original set --
      // `authoritative_closure_processed` alone under-counts here because it
      // holds only the unsatisfied-and-processed subset, never the
      // already-represented targets counted in the loop above.
      stitch_metrics.authoritative_exact_target_count =
          static_cast<std::uint32_t>(authoritative_obligations.size());
      for (const auto target : authoritative_closure_processed)
        if (!authoritative_obligations.contains(target)) ++stitch_metrics.authoritative_exact_target_count;
      for (const auto target : authoritative_closure_processed) {
        if (unsatisfied_targets.contains(target)) {
          // Already accounted for in the original obligation loop above.
        } else if (retained_entries_initial.contains(target)) {
          // A recursively-discovered obligation that happened to already be
          // a retained entry before this closure pass even began.
          ++stitch_metrics.authoritative_exact_targets_already_represented;
          continue;
        }
        if (retained_entries_final.contains(target)) {
          ++stitch_metrics.authoritative_exact_targets_materialized;
        } else {
          authoritative_unsatisfied_targets.insert(target);
        }
      }
    }
    // SEG-007-T227: an ordinary fallthrough into a validated semantic-partition
    // boundary is an executable obligation only if its destination has one of
    // the existing representations.  Unlike the exact-control closure above,
    // this deliberately does not manufacture a frontier: an ordinary PC
    // advance has no independent source-provenanced failure to report.  The
    // earliest compatible repair is therefore D: split an already decoded
    // body at the boundary and let the ordinary block construction and pruning
    // rules decide whether that new entry survives.  Boundary membership alone
    // remains neither an entry nor a dispatch arm.
    //
    // Work from the final live prefix produced by the preceding existing
    // closures.  The sorted set makes duplicate nominations deterministic;
    // exact decoded-map membership is the alignment/provenance check, and no
    // new walk, edge, root, capacity, or runtime mechanism is introduced.
    while (prefix) {
      std::set<Address> retained_entries;
      std::set<Address> retained_instructions;
      for (const auto &block : prefix->static_blocks) {
        retained_entries.insert(block.id.entry.value);
        for (const auto &instruction : block.instructions)
          retained_instructions.insert(instruction.source.address.value);
      }
      std::set<Address> ordinary_fallthrough_boundary_nominations;
      for (const auto &edge : prefix->static_edges) {
        if (edge.kind != M68kStaticEdgeKind::fallthrough ||
            !retained_instructions.contains(edge.source_instruction.source.address.value) ||
            retained_entries.contains(edge.target.value) ||
            !is_semantic_partition_boundary_address(analysis, edge.target.value) ||
            !merged_decoded.contains(edge.target.value))
          continue;
        ordinary_fallthrough_boundary_nominations.insert(edge.target.value);
      }
      bool split_required = false;
      for (const auto target : ordinary_fallthrough_boundary_nominations)
        if (block_entries.insert(target).second) {
          block_entry_order.push_back(target);
          split_required = true;
        }
      // Each successful split can expose another already-decoded, non-entry
      // boundary at the same seam.  Rebuild until the finite leader set stops
      // growing; every iteration adds at least one address to `block_entries`,
      // so this is a deterministic finite-set fixed point, not a new budgeted
      // discovery loop.  A target that is already a leader but is later pruned
      // is intentionally not retried here: that is the distinct B/lifecycle
      // case and remains subject to the existing fail-closed representation
      // rules.
      if (!split_required) break;
      prefix = build_analysis(true, true);
      if (!prefix) return reject();
    }
    // SEG-007-T228/T229 / ADR-0028 §8, §8.3: a final live ordinary
    // `fallthrough` or `return_to_continuation` relation whose already-led
    // destination was itself removed by the completed-prefix erase-pass
    // fixpoint (orphan-return or unsafe-outgoing-edge pruning, or an
    // equivalent validity check) is destination-invalid under every live
    // predecessor -- both pruning checks are properties of the destination
    // block's OWN terminal instruction/own outgoing edges, never of which
    // specific predecessor reaches it. This reuses the exact same
    // destination-global `known_but_unemitted_target` representation §9's
    // exact-control closure already builds below for a direct-control target
    // pruned the same way: every live predecessor (this fallthrough/return
    // edge, another retained edge already represented separately, or an
    // independent root) observes the identical destination representation,
    // never a per-predecessor stop. A `fallthrough_continuation` edge into a
    // pruned target is not added here: it is already unconditionally dropped
    // as a dangling duplicate above (its (source, target) pair is always
    // either covered by another retained edge kind or the synthesized
    // block-merge shape build_analysis already emits as plain `fallthrough`),
    // so it carries no separate live relation of its own to represent.
    // Computed from the FINAL prefix (after the fallthrough-boundary split
    // fixed point above), so a target the split promoted to an ordinary
    // retained block is correctly excluded; folded into the exact same
    // `authoritative_unsatisfied_targets` set so it flows through §9's
    // existing materialize-or-represent-or-fail-closed Phase B resolution
    // below, adding no new set, dispatcher, or edge kind.
    //
    // SEG-007-T229: `return_to_continuation` is folded in alongside plain
    // `fallthrough` for the exact same destination-invalid reason. T228 only
    // covered `fallthrough`; a live call frame's own continuation can be
    // decoded, merged, and even independently nominated as its own
    // fallthrough-continuation/resolved-control-target root (`analysis.
    // semantic_partition_boundary_addresses`) and STILL fail to survive the
    // erase-pass fixpoint as a retained block for the same destination-
    // intrinsic reasons. `runtime_frontier_eligible`'s own edge-target check
    // (platforms/genesis/machine/src/frontend.cpp) treats bare `semantic_partition_
    // boundary` membership as a sufficient (non-retained) edge target -- by
    // design, so a boundary root's block can be validated before its own
    // Phase-2 walk has necessarily been folded back into this exact prefix --
    // but C4 emission (libs/codegen/c11/src/frontend.cpp) never grants that same
    // exemption: it only ever dispatches to an actual retained block or an
    // actual emitted frontier stop. Without this fold, such a
    // `return_to_continuation` edge survives analysis (silently, via the
    // boundary-membership exemption) only to be rejected much later at
    // codegen with no typed diagnostic at all. Folding it here closes that
    // exact representation gap through the SAME existing mechanism, with no
    // new architecture: a genuinely unrepresentable target still fails
    // closed via §9.4 below (never silently dropped), and one already
    // covered by the ordinary primary/secondary discovery-issue frontier
    // machinery is recognized as already satisfied by §9's own dedup.
    if (prefix) {
      std::set<Address> retained_entries_for_pruned_fallthrough;
      std::set<Address> retained_instructions_for_pruned_fallthrough;
      for (const auto &block : prefix->static_blocks) {
        retained_entries_for_pruned_fallthrough.insert(block.id.entry.value);
        for (const auto &instruction : block.instructions)
          retained_instructions_for_pruned_fallthrough.insert(instruction.source.address.value);
      }
      for (const auto &edge : prefix->static_edges) {
        if (edge.kind != M68kStaticEdgeKind::fallthrough &&
            edge.kind != M68kStaticEdgeKind::return_to_continuation)
          continue;
        if (edge.target.space != TargetAddressSpace::m68k_program) continue;
        if (!retained_instructions_for_pruned_fallthrough.contains(
                edge.source_instruction.source.address.value))
          continue;
        if (retained_entries_for_pruned_fallthrough.contains(edge.target.value)) continue;
        if (!merged_decoded.contains(edge.target.value)) continue;
        authoritative_unsatisfied_targets.insert(edge.target.value);
      }
    }
    // SEG-007-T064: the exact same allowed-edge-target set build_analysis
    // just used to decide block retention, threaded into every
    // runtime_frontier_eligible call below so a block build_analysis
    // retained (because one of its edges targets a candidate address) is
    // never then rejected here purely because this specific call's own
    // single-address view does not separately recognize a sibling target.
    const auto sibling_addresses = compute_candidate_frontier_addresses();
    if (!frontier_class || !prefix ||
        !runtime_frontier_eligible(*prefix, *failure, *frontier_class, frontier_access, sibling_addresses,
                                    analysis.semantic_partition_boundary_addresses))
      return reject();
    std::vector<UnresolvedFrontier> frontiers;
    frontiers.push_back({*frontier_class, *failure, frontier_access});
    // SEG-007-T064: every best-effort secondary is retained precisely when
    // it independently satisfies runtime_frontier_eligible under its own
    // classified class; otherwise it is retained as known_but_unemitted_
    // target when *that* class's (weaker) eligibility test passes; otherwise
    // it is dropped -- never force-fit into a precise class it doesn't
    // actually match, and never allowed to fail the whole promotion the
    // primary failure already earned.
    for (auto &secondary : secondary_failures) {
      auto &secondary_failure = secondary.first;
      auto &secondary_access = secondary.second;
      const auto secondary_class = classify_frontier(secondary_failure, secondary_access.has_value());
      if (secondary_class && runtime_frontier_eligible(*prefix, secondary_failure, *secondary_class,
                                                        secondary_access, sibling_addresses,
                                                        analysis.semantic_partition_boundary_addresses)) {
        frontiers.push_back({*secondary_class, secondary_failure, secondary_access});
        continue;
      }
      if (runtime_frontier_eligible(*prefix, secondary_failure, GenesisFrontierClass::known_but_unemitted_target,
                                     secondary_access, sibling_addresses,
                                     analysis.semantic_partition_boundary_addresses))
        frontiers.push_back({GenesisFrontierClass::known_but_unemitted_target, secondary_failure, secondary_access});
    }
    // SEG-007-T214 / ADR-0028 §9 -- PHASE B: the final per-target
    // satisfied / typed-frontier / fail-closed resolution (§9.1/§9.6),
    // deferred to here so an obligated target already represented through
    // the ordinary, pre-existing primary/secondary discovery-issue frontier
    // mechanism above (constructed directly from the walker's own
    // `M68kDiscoveryIssue`, independent of whether the target was ever
    // decoded) counts as satisfied without this closure needing to
    // construct anything of its own.
    if (!authoritative_unsatisfied_targets.empty()) {
      std::set<Address> already_represented_addresses;
      for (const auto &represented : frontiers)
        if (represented.diagnostic.provenance)
          already_represented_addresses.insert(represented.diagnostic.provenance->source.address.value);
      std::set<Address> still_unsatisfied_targets;
      for (const auto target : authoritative_unsatisfied_targets)
        if (!already_represented_addresses.contains(target)) still_unsatisfied_targets.insert(target);
      // §9.6: every closure-constructed target's own eligibility check must
      // tolerate every OTHER already-represented sibling address (both the
      // ordinary primary/secondary discovery-issue frontiers already folded
      // into `frontiers`, and every other closure target still pending in
      // this same pass) as a safe non-retained edge destination -- mirroring
      // the ordinary `sibling_addresses` mechanism above -- so a genuine,
      // otherwise-valid retained edge into one of those addresses (e.g. a
      // `return_to_continuation` edge back into the primary frontier's own
      // mid-block address) is never mistaken for an unsafe dangling edge
      // merely because THIS check's own single-address view does not
      // separately recognize it.
      std::set<Address> closure_sibling_addresses = already_represented_addresses;
      closure_sibling_addresses.insert(still_unsatisfied_targets.begin(), still_unsatisfied_targets.end());
      bool closure_failed_closed = false;
      for (const auto target : still_unsatisfied_targets) {
        // Materialize-then-reprune case (§9.1/§9.5), or a validation failure
        // for a target that nonetheless has a valid decoded provenance to
        // anchor an actual typed frontier-stop representation. A target with
        // NO decoded provenance at all cannot be given one (§9.6's vocabulary
        // requires an actual `UnresolvedFrontier` with valid provenance, never
        // a fabricated one) and falls through to the fail-closed count below.
        const auto decoded = merged_decoded.find(target);
        bool represented = false;
        if (decoded != merged_decoded.end()) {
          M68kDiscoveryIssue issue{};
          issue.category = DirectFlowDiagnostic::reached_unresolved_direct_edge;
          issue.address = M68kProgramAddress{TargetAddressSpace::m68k_program, target};
          issue.provenance = decoded->second.provenance;
          issue.instruction_length = decoded->second.provenance.length.value;
          issue.reconstruct_instruction_read_access = true;
          issue.reconstruct_single_mapping_claim = true;
          const auto closure_diagnostic = translate_m68k_discovery_issue(program, issue);
          if (runtime_frontier_eligible(*prefix, closure_diagnostic, GenesisFrontierClass::known_but_unemitted_target,
                                         std::nullopt, closure_sibling_addresses,
                                         analysis.semantic_partition_boundary_addresses)) {
            frontiers.push_back({GenesisFrontierClass::known_but_unemitted_target, closure_diagnostic, std::nullopt});
            authoritative_exact_closure_represented_addresses.insert(target);
            ++stitch_metrics.authoritative_exact_targets_frontier_represented;
            represented = true;
          }
        }
        if (!represented) {
          ++stitch_metrics.authoritative_exact_targets_rejected;
          closure_failed_closed = true;
        }
      }
      // §9.4: the final generation-time backstop verifier. Independent of
      // §9.1-§9.3's primary repair mechanism, generation is rejected before
      // native output when any obligated target could be neither
      // materialized/promoted nor given an actual typed frontier
      // representation -- never a silently emitted dangling edge.
      if (closure_failed_closed) {
        stitch_metrics.authoritative_exact_targets_unrepresented = stitch_metrics.authoritative_exact_targets_rejected;
        auto r = rejected(DirectFlowDiagnostic::authoritative_exact_target_closure_exhausted, program);
        set_source(r, entry);
        return r;
      }
    }
    // ADR-0038 / SEG-007-T234: rooted representation-then-retention closure
    // over the authoritative erased-entry graph, layered on top of the
    // completed-prefix transaction that has just finished feeding ADR-0028
    // §9 above. Candidate B: a deterministic, transactional, whole-component
    // peel. Generic -- no Sonic-specific address, heuristic, or allowlist;
    // driven only by `prefix`/`merged_decoded`/`merged_edges`/`block_entries`
    // already built above.
    if (prefix) {
      const auto pre_adr0038_prefix_block_count = prefix->static_blocks.size();
      const auto *adr0038_test_control =
          program.image.source_id.starts_with("synthetic/") && program.adr0038_synthetic_test_control
              ? &*program.adr0038_synthetic_test_control
              : nullptr;
      // Step 1: consume only identities captured by the completed-prefix
      // pruning transaction itself. The capture is populated inside the
      // erase predicate, from the actual block object being erased and the
      // exact unsafe relations which made that invocation return true. No
      // leader/decoded/boundary membership, span reconstruction, endpoint
      // expansion, or second discovery walk participates in graph identity.
      std::set<Address> erased_entries;
      for (const auto &fact : adr0038_erase_transaction.facts) erased_entries.insert(fact.entry);
      std::set<Address> adr0038_graph_nodes;
      std::map<Address, std::vector<Address>> adr0038_graph_edges;
      for (const auto &fact : adr0038_erase_transaction.facts) {
        if (fact.reason != CompletedPrefixEraseReason::unsafe_outgoing_relation) continue;
        for (const auto &relation : fact.unsafe_relations) {
          if (relation.edge.target.space != TargetAddressSpace::m68k_program ||
              !erased_entries.contains(relation.edge.target.value))
            continue;
          adr0038_graph_edges[relation.source_entry].push_back(relation.edge.target.value);
          adr0038_graph_nodes.insert(relation.source_entry);
          adr0038_graph_nodes.insert(relation.edge.target.value);
        }
      }
      if (adr0038_test_control != nullptr)
        for (const auto &[source, target] : adr0038_test_control->additional_exact_erased_unsafe_relations)
          if (erased_entries.contains(source) && erased_entries.contains(target)) {
            adr0038_graph_edges[source].push_back(target);
            adr0038_graph_nodes.insert(source);
            adr0038_graph_nodes.insert(target);
          }
      stitch_metrics.adr0038_graph_node_count = static_cast<std::uint32_t>(adr0038_graph_nodes.size());
      std::uint32_t adr0038_edge_count = 0U;
      for (const auto &[source, targets] : adr0038_graph_edges) adr0038_edge_count += static_cast<std::uint32_t>(targets.size());
      stitch_metrics.adr0038_graph_edge_count = adr0038_edge_count;
      std::map<Address, std::uint32_t> adr0038_indegree;
      for (const auto node : adr0038_graph_nodes) adr0038_indegree.emplace(node, 0U);
      for (const auto &[source, targets] : adr0038_graph_edges)
        for (const auto target : targets) ++adr0038_indegree[target];
      for (const auto node : adr0038_graph_nodes) {
        if (adr0038_indegree[node] == 0U) ++stitch_metrics.adr0038_graph_root_count;
        const auto outgoing = adr0038_graph_edges.find(node);
        if (outgoing == adr0038_graph_edges.end() || outgoing->second.empty())
          ++stitch_metrics.adr0038_graph_leaf_count;
      }
      if (!adr0038_graph_nodes.empty()) {
        // Step 1 (acyclicity): a deterministic three-color DFS. A cycle fails
        // the WHOLE transaction closed before any representation/retention
        // mutation happens below -- nothing has been mutated yet at this
        // point, so "restoring the pre-ADR-0038 prefix and frontier exactly"
        // is simply "do nothing further".
        enum class VisitState : std::uint8_t { unvisited, visiting, done };
        std::map<Address, VisitState> visit_state;
        for (const auto node : adr0038_graph_nodes) visit_state.emplace(node, VisitState::unvisited);
        std::vector<Address> reverse_topological_order;
        bool cyclic = false;
        const std::function<void(Address)> visit = [&](Address node) {
          if (cyclic) return;
          visit_state[node] = VisitState::visiting;
          const auto found = adr0038_graph_edges.find(node);
          if (found != adr0038_graph_edges.end()) {
            for (const auto target : found->second) {
              if (visit_state[target] == VisitState::visiting) { cyclic = true; return; }
              if (visit_state[target] == VisitState::unvisited) visit(target);
              if (cyclic) return;
            }
          }
          visit_state[node] = VisitState::done;
          reverse_topological_order.push_back(node);
        };
        for (const auto node : adr0038_graph_nodes) {
          if (cyclic) break;
          if (visit_state[node] == VisitState::unvisited) visit(node);
        }
        if (cyclic) {
          stitch_metrics.adr0038_cyclic = 1U;
        } else {
          std::map<Address, std::uint32_t> depth;
          for (auto node = reverse_topological_order.rbegin(); node != reverse_topological_order.rend(); ++node) {
            const auto found = adr0038_graph_edges.find(*node);
            if (found != adr0038_graph_edges.end())
              for (const auto target : found->second)
                depth[target] = std::max(depth[target], static_cast<std::uint32_t>(depth[*node] + 1U));
            stitch_metrics.adr0038_graph_max_depth =
                std::max(stitch_metrics.adr0038_graph_max_depth, depth[*node]);
          }
          // Steps 2-6: the deterministic rooted fixed point. `forbidden`
          // monotonically grows every productive round (bounded by the
          // finite `adr0038_graph_nodes` size), which is both the
          // termination proof and the "bounded monotone progress" ADR-0038
          // step 6 requires. `reverse_topological_order` fixes deterministic
          // insertion order for `block_entry_order` across every round.
          const auto saved_block_entries = block_entries;
          const auto saved_block_entry_order = block_entry_order;
          const auto saved_boundary = analysis.semantic_partition_boundary_addresses;
          std::set<Address> forbidden;
          std::set<Address> ordinarily_pruned;
          bool ceiling_exhausted = false;
          bool total_failure = false;
          std::optional<FrontendAnalysis> converged_prefix;
          std::set<Address> converged_ordinary_retained;
          std::set<Address> converged_typed_frontier_nodes;
          // Reverse-topological classification starts with exactly the graph
          // leaves: only a non-retained destination can provisionally own a
          // destination-global stop. Interior nodes are first offered as
          // executable entries; ordinary pruning then decides whether their
          // dependency chain reaches one of these represented leaves.
          std::set<Address> proposals;
          std::set<Address> preexisting_frontier_addresses;
          for (const auto &represented : frontiers)
            if (represented.diagnostic.provenance)
              preexisting_frontier_addresses.insert(represented.diagnostic.provenance->source.address.value);
          for (const auto node : adr0038_graph_nodes) {
            const auto outgoing = adr0038_graph_edges.find(node);
            if ((outgoing == adr0038_graph_edges.end() || outgoing->second.empty()) &&
                !preexisting_frontier_addresses.contains(node))
              proposals.insert(node);
          }
          std::uint32_t rounds = 0U;
          std::uint32_t peeled_component_count = 0U;
          std::vector<std::uint32_t> peeled_component_sizes;
          std::set<Address> peeled_nodes;
          for (;;) {
            std::vector<Address> active_ordered;
            for (const auto node : reverse_topological_order)
              if (!forbidden.contains(node) && !ordinarily_pruned.contains(node)) active_ordered.push_back(node);
            if (active_ordered.empty()) {
              converged_prefix = prefix;
              break;
            }
            ++rounds;
            const auto closure_round_ceiling =
                adr0038_test_control != nullptr && adr0038_test_control->closure_round_ceiling
                    ? *adr0038_test_control->closure_round_ceiling
                    : m68k_transitive_pruned_direct_control_closure_round_ceiling;
            if (rounds > closure_round_ceiling) {
              ceiling_exhausted = true;
              break;
            }
            // Every retry is a scratch rebuild from the exact pre-ADR-0038
            // leader and boundary state. Proposals are supplied only through
            // build_analysis's existing represented-target checks; unlike
            // candidate frontier membership, they never suppress construction.
            block_entries = saved_block_entries;
            block_entry_order = saved_block_entry_order;
            analysis.semantic_partition_boundary_addresses = saved_boundary;
            for (const auto node : forbidden) {
              block_entries.erase(node);
              std::erase(block_entry_order, node);
              analysis.semantic_partition_boundary_addresses.erase(node);
            }
            for (const auto node : active_ordered) {
              if (block_entries.insert(node).second) block_entry_order.push_back(node);
            }
            std::set<Address> unreachable_retained;
            const std::set<Address> active_entries(active_ordered.begin(), active_ordered.end());
            std::set<Address> provisional_safe_addresses = proposals;
            provisional_safe_addresses.insert(preexisting_frontier_addresses.begin(),
                                               preexisting_frontier_addresses.end());
            auto trial = build_analysis(true, true, nullptr, &provisional_safe_addresses,
                                        &unreachable_retained, &active_entries);
            if (!trial) {
              total_failure = true;
              break;
            }
            if (adr0038_test_control != nullptr)
              for (const auto entry : adr0038_test_control->additionally_unreachable_retained_entries)
                if (std::any_of(trial->static_blocks.begin(), trial->static_blocks.end(), [&](const auto &block) {
                      return block.id.entry.value == entry;
                    }))
                  unreachable_retained.insert(entry);
            std::set<Address> retained_trial_entries;
            for (const auto &block : trial->static_blocks) retained_trial_entries.insert(block.id.entry.value);
            std::set<Address> ordinary_retained;
            for (const auto node : active_ordered) {
              if (retained_trial_entries.contains(node)) ordinary_retained.insert(node);
            }

            // ADR-0038 step 5: partition the whole unreachable provisional
            // retained program into weak components using the exact retained
            // edges and M1b call-continuation relation. Every graph member in
            // every such component is forbidden atomically; a proposal owned
            // only by sources in that component is removed in the same step.
            std::set<Address> newly_forbidden;
            std::map<Address, Address> instruction_owner;
            for (const auto &block : trial->static_blocks)
              for (const auto &instruction : block.instructions)
                instruction_owner.emplace(instruction.source.address.value, block.id.entry.value);
            std::map<Address, std::set<Address>> component_adjacency;
            const auto connect = [&](Address source_instruction, Address target) {
              const auto owner = instruction_owner.find(source_instruction);
              if (owner == instruction_owner.end() || !unreachable_retained.contains(owner->second) ||
                  !unreachable_retained.contains(target))
                return;
              component_adjacency[owner->second].insert(target);
              component_adjacency[target].insert(owner->second);
            };
            for (const auto &edge : trial->static_edges)
              connect(edge.source_instruction.source.address.value, edge.target.value);
            for (const auto &frame : trial->static_frames)
              connect(frame.call.caller.source.address.value,
                      static_cast<Address>(frame.call.caller.source.address.value + frame.call.caller.length.value));
            std::set<Address> visited_unreachable;
            for (const auto seed : unreachable_retained) {
              if (!visited_unreachable.insert(seed).second) continue;
              std::set<Address> component{seed};
              std::vector<Address> pending{seed};
              for (std::size_t index = 0; index < pending.size(); ++index) {
                for (const auto next : component_adjacency[pending[index]])
                  if (visited_unreachable.insert(next).second) {
                    component.insert(next);
                    pending.push_back(next);
                  }
              }
              newly_forbidden.insert(component.begin(), component.end());
              ++peeled_component_count;
              peeled_component_sizes.push_back(static_cast<std::uint32_t>(component.size()));
              peeled_nodes.insert(component.begin(), component.end());
              for (const auto proposal : proposals) {
                bool has_owner = false;
                bool component_only = true;
                for (const auto &[source, targets] : adr0038_graph_edges)
                  if (std::find(targets.begin(), targets.end(), proposal) != targets.end()) {
                    has_owner = true;
                    if (!component.contains(source)) component_only = false;
                  }
                if (has_owner && component_only) newly_forbidden.insert(proposal);
              }
            }
            if (!newly_forbidden.empty()) {
              forbidden.insert(newly_forbidden.begin(), newly_forbidden.end());
              for (const auto node : newly_forbidden) proposals.erase(node);
              // The component itself is peeled atomically, then a scratch
              // rebuild with the ordinary reachability erase enabled removes
              // newly disconnected dependent ancestors. Those ordinary
              // consequences are not additional "peeled components" and must
              // not be protected merely so a later round can count them again.
              block_entries = saved_block_entries;
              block_entry_order = saved_block_entry_order;
              analysis.semantic_partition_boundary_addresses = saved_boundary;
              std::set<Address> remaining_active;
              for (const auto node : reverse_topological_order)
                if (!forbidden.contains(node) && !ordinarily_pruned.contains(node)) {
                  remaining_active.insert(node);
                  if (block_entries.insert(node).second) block_entry_order.push_back(node);
                }
              std::set<Address> ordinary_safe_addresses = proposals;
              ordinary_safe_addresses.insert(preexisting_frontier_addresses.begin(),
                                             preexisting_frontier_addresses.end());
              auto ordinarily_pruned_trial =
                  build_analysis(true, true, nullptr, &ordinary_safe_addresses, nullptr, nullptr);
              if (!ordinarily_pruned_trial) {
                total_failure = true;
                break;
              }
              std::set<Address> ordinary_trial_entries;
              for (const auto &block : ordinarily_pruned_trial->static_blocks)
                ordinary_trial_entries.insert(block.id.entry.value);
              for (const auto node : remaining_active)
                if (!ordinary_trial_entries.contains(node)) ordinarily_pruned.insert(node);
              continue;
            }

            // Step 6 proposal ownership: only a non-retained destination of
            // an exact graph relation sourced in the rooted retained program
            // remains provisionally represented.
            std::set<Address> desired_proposals;
            for (const auto &[source, targets] : adr0038_graph_edges) {
              if (!retained_trial_entries.contains(source) || unreachable_retained.contains(source)) continue;
              for (const auto target : targets)
                if (!forbidden.contains(target) && !retained_trial_entries.contains(target))
                  if (!preexisting_frontier_addresses.contains(target)) desired_proposals.insert(target);
            }
            // Reverse-topological classification is monotone: a proposal may
            // lose its rooted retained owner, but an interior node which was
            // not classified as a leaf cannot become a proposal merely because
            // a later scratch rebuild pruned it. Adding such a node back is the
            // two-state proposal/retention oscillation the closure ceiling is
            // intended to catch, not convergence. Its dependent ancestors must
            // instead fall out under the next normal rebuild.
            std::erase_if(desired_proposals, [&](Address node) { return !proposals.contains(node); });
            if (desired_proposals != proposals) {
              proposals = std::move(desired_proposals);
              continue;
            }

            std::set<Address> round_sibling_addresses = sibling_addresses;
            round_sibling_addresses.insert(proposals.begin(), proposals.end());
            for (const auto node : proposals) {
              const auto decoded = merged_decoded.find(node);
              if (decoded == merged_decoded.end()) { total_failure = true; break; }
              M68kDiscoveryIssue issue{};
              issue.category = DirectFlowDiagnostic::reached_unresolved_direct_edge;
              issue.address = M68kProgramAddress{TargetAddressSpace::m68k_program, node};
              issue.provenance = decoded->second.provenance;
              issue.instruction_length = decoded->second.provenance.length.value;
              issue.reconstruct_instruction_read_access = true;
              issue.reconstruct_single_mapping_claim = true;
              const auto diagnostic = translate_m68k_discovery_issue(program, issue);
              if (!runtime_frontier_eligible(*trial, diagnostic,
                                             GenesisFrontierClass::known_but_unemitted_target, std::nullopt,
                                             round_sibling_addresses,
                                             analysis.semantic_partition_boundary_addresses)) {
                total_failure = true;
                break;
              }
            }
            if (total_failure) break;
            converged_prefix = trial;
            converged_ordinary_retained = std::move(ordinary_retained);
            converged_typed_frontier_nodes = proposals;
            break;
          }
          if (ceiling_exhausted || total_failure) {
            stitch_metrics.adr0038_ceiling_exhausted = ceiling_exhausted ? 1U : 0U;
            block_entries = saved_block_entries;
            block_entry_order = saved_block_entry_order;
            analysis.semantic_partition_boundary_addresses = saved_boundary;
          } else if (converged_prefix && (!converged_ordinary_retained.empty() || !converged_typed_frontier_nodes.empty())) {
            // Step 7: final validation + atomic commit. Every retained
            // block-entry addition is already rooted (build_analysis's own
            // internal M1b reachability erase pass would have dropped it
            // otherwise); every typed proposal has already passed unmodified
            // `runtime_frontier_eligible` above against this exact converged
            // prefix. Commit the grown prefix and every destination-global
            // frontier together.
            prefix = converged_prefix;
            std::set<Address> retained_instruction_addresses;
            for (const auto &block : prefix->static_blocks)
              for (const auto &instruction : block.instructions)
                retained_instruction_addresses.insert(instruction.source.address.value);
            std::erase_if(frontiers, [&](const UnresolvedFrontier &frontier) {
              return frontier.diagnostic.provenance &&
                     retained_instruction_addresses.contains(frontier.diagnostic.provenance->source.address.value);
            });
            for (const auto node : converged_typed_frontier_nodes) {
              const auto decoded = merged_decoded.find(node);
              if (decoded == merged_decoded.end()) continue;
              M68kDiscoveryIssue issue{};
              issue.category = DirectFlowDiagnostic::reached_unresolved_direct_edge;
              issue.address = M68kProgramAddress{TargetAddressSpace::m68k_program, node};
              issue.provenance = decoded->second.provenance;
              issue.instruction_length = decoded->second.provenance.length.value;
              issue.reconstruct_instruction_read_access = true;
              issue.reconstruct_single_mapping_claim = true;
              const auto diagnostic = translate_m68k_discovery_issue(program, issue);
              frontiers.push_back({GenesisFrontierClass::known_but_unemitted_target, diagnostic, std::nullopt});
              authoritative_exact_closure_represented_addresses.insert(node);
            }
            // The retained graph can supersede or disconnect an older outer
            // frontier. Revalidate the complete provisional frontier vector
            // against the exact converged prefix and destination-global
            // sibling set; stale facts are removed, never force-fit. The
            // existing retained-edge consistency backstop below still rejects
            // any removal that would leave a live edge unrepresented.
            std::set<Address> converged_frontier_addresses;
            for (const auto &frontier : frontiers)
              if (frontier.diagnostic.provenance)
                converged_frontier_addresses.insert(frontier.diagnostic.provenance->source.address.value);
            std::erase_if(frontiers, [&](const UnresolvedFrontier &frontier) {
              return !runtime_frontier_eligible(*prefix, frontier.diagnostic, frontier.class_, frontier.access,
                                                converged_frontier_addresses,
                                                analysis.semantic_partition_boundary_addresses);
            });
            stitch_metrics.adr0038_closure_rounds = rounds;
            stitch_metrics.adr0038_peeled_component_count = peeled_component_count;
            std::sort(peeled_component_sizes.begin(), peeled_component_sizes.end());
            stitch_metrics.adr0038_peeled_component_sizes = peeled_component_sizes;
            stitch_metrics.adr0038_peeled_node_count = static_cast<std::uint32_t>(peeled_nodes.size());
            stitch_metrics.adr0038_retained_block_delta =
                static_cast<std::int64_t>(prefix->static_blocks.size()) -
                static_cast<std::int64_t>(pre_adr0038_prefix_block_count);
            stitch_metrics.adr0038_ordinary_retained_graph_node_count =
                static_cast<std::uint32_t>(converged_ordinary_retained.size());
            stitch_metrics.adr0038_typed_frontier_count =
                static_cast<std::uint32_t>(converged_typed_frontier_nodes.size());
            stitch_metrics.adr0038_unresolved_node_count =
                stitch_metrics.adr0038_graph_node_count -
                stitch_metrics.adr0038_ordinary_retained_graph_node_count -
                stitch_metrics.adr0038_typed_frontier_count;
          } else {
            // No rooted surviving component: every graph node ended up
            // forbidden. `block_entries`/`block_entry_order`/the boundary set
            // already returned to their exact pre-ADR-0038 values as a side
            // effect of forbidding every node above; restated explicitly here
            // so this branch is exact and self-contained regardless of that
            // per-round bookkeeping.
            block_entries = saved_block_entries;
            block_entry_order = saved_block_entry_order;
            analysis.semantic_partition_boundary_addresses = saved_boundary;
            stitch_metrics.adr0038_closure_rounds = rounds;
            stitch_metrics.adr0038_peeled_component_count = peeled_component_count;
            std::sort(peeled_component_sizes.begin(), peeled_component_sizes.end());
            stitch_metrics.adr0038_peeled_component_sizes = peeled_component_sizes;
            stitch_metrics.adr0038_peeled_node_count = static_cast<std::uint32_t>(peeled_nodes.size());
            stitch_metrics.adr0038_unresolved_node_count = stitch_metrics.adr0038_graph_node_count;
          }
        }
      }
    }
    // SEG-007-T183 / ADR-0028 §8: separate the complete, untruncated semantic
    // frontier obligation set from the bounded diagnostic projection.
    // `frontiers` is reassigned to the COMPLETE set (never truncated) -- this
    // is what downstream C4 emission actually iterates -- while the bounded
    // subset and residual count feed instrumentation only.
    // `m68k_discovery_max_frontier_exits` no longer fails the whole
    // translation merely because the deduplicated count exceeds it.
    auto projection = recompiler_sort_dedup_and_project_frontiers(
        std::move(frontiers), m68k_discovery_max_frontier_exits, frontier_sort_key);
    stitch_metrics.unresolved_semantic_frontier_count_before_bounding =
        static_cast<std::uint32_t>(projection.complete.size());
    stitch_metrics.diagnostic_frontier_count_after_bounding =
        static_cast<std::uint32_t>(projection.bounded.size());
    stitch_metrics.residual_frontier_obligation_count = projection.residual_count;
    frontiers = std::move(projection.complete);
    // SEG-007-T183 / ADR-0028 §8: a second normalized stderr instrumentation
    // line, printed once this partial promotion's own semantic-partition-
    // boundary/frontier-projection metrics are all known (they depend on
    // `build_analysis`/this projection, both of which run after the first
    // ADR-0026/0027/0028 stitch-metrics line above). Numbers/booleans only
    // -- never a raw address. Guarded on the same non-empty-inventory
    // condition as the first line, and parsed the same generic key=value way
    // by tools/genesis_startup_bridge.py.
    if (stitch_metrics.offline_candidate_count != 0U)
      std::fprintf(stderr,
                   "segarecomp: offline inventory partition: semantic_partition_boundary_count=%u "
                   "unresolved_semantic_frontier_count_before_bounding=%u "
                   "diagnostic_frontier_count_after_bounding=%u residual_frontier_obligation_count=%u "
                   "retained_block_count_before_pruning=%u retained_block_count_after_pruning=%u "
                   "ingress_retained=%u late_indirect_closure_rounds=%u "
                   "late_indirect_target_unit_count=%u late_indirect_closure_converged=%u "
                   "authoritative_exact_target_count=%u "
                   "authoritative_exact_targets_already_represented=%u "
                   "authoritative_exact_targets_materialized=%u "
                   "authoritative_exact_targets_frontier_represented=%u "
                   "authoritative_exact_targets_rejected=%u "
                   "authoritative_exact_closure_rounds=%u "
                   "authoritative_exact_targets_unrepresented=%u "
                   "adr0038_graph_node_count=%u adr0038_graph_edge_count=%u adr0038_graph_root_count=%u "
                   "adr0038_graph_leaf_count=%u adr0038_graph_max_depth=%u adr0038_cyclic=%u "
                   "adr0038_closure_rounds=%u adr0038_peeled_component_count=%u "
                    "adr0038_peeled_node_count=%u adr0038_retained_block_delta=%lld "
                    "adr0038_ordinary_retained_graph_node_count=%u "
                   "adr0038_typed_frontier_count=%u adr0038_unresolved_node_count=%u "
                    "adr0038_ceiling_exhausted=%u",
                   stitch_metrics.semantic_partition_boundary_count,
                   stitch_metrics.unresolved_semantic_frontier_count_before_bounding,
                   stitch_metrics.diagnostic_frontier_count_after_bounding,
                   stitch_metrics.residual_frontier_obligation_count,
                   stitch_metrics.retained_block_count_before_pruning,
                   stitch_metrics.retained_block_count_after_pruning,
                   stitch_metrics.ingress_retained ? 1U : 0U,
                   stitch_metrics.late_indirect_closure_rounds,
                   stitch_metrics.late_indirect_target_unit_count,
                   stitch_metrics.late_indirect_closure_converged ? 1U : 0U,
                   stitch_metrics.authoritative_exact_target_count,
                   stitch_metrics.authoritative_exact_targets_already_represented,
                   stitch_metrics.authoritative_exact_targets_materialized,
                   stitch_metrics.authoritative_exact_targets_frontier_represented,
                   stitch_metrics.authoritative_exact_targets_rejected,
                   stitch_metrics.authoritative_exact_closure_rounds,
                   stitch_metrics.authoritative_exact_targets_unrepresented,
                   stitch_metrics.adr0038_graph_node_count, stitch_metrics.adr0038_graph_edge_count,
                   stitch_metrics.adr0038_graph_root_count, stitch_metrics.adr0038_graph_leaf_count,
                   stitch_metrics.adr0038_graph_max_depth, stitch_metrics.adr0038_cyclic,
                   stitch_metrics.adr0038_closure_rounds,
                   stitch_metrics.adr0038_peeled_component_count, stitch_metrics.adr0038_peeled_node_count,
                    static_cast<long long>(stitch_metrics.adr0038_retained_block_delta),
                    stitch_metrics.adr0038_ordinary_retained_graph_node_count,
                    stitch_metrics.adr0038_typed_frontier_count,
                    stitch_metrics.adr0038_unresolved_node_count, stitch_metrics.adr0038_ceiling_exhausted);
    if (stitch_metrics.offline_candidate_count != 0U) {
      std::fprintf(stderr, " adr0038_peeled_component_sizes=[");
      for (std::size_t index = 0; index < stitch_metrics.adr0038_peeled_component_sizes.size(); ++index)
        std::fprintf(stderr, "%s%u", index == 0U ? "" : ",",
                     stitch_metrics.adr0038_peeled_component_sizes[index]);
      std::fprintf(stderr, "]\n");
    }
    // SEG-007-T064: `sibling_addresses` is exactly the set build_analysis
    // treated as safe non-retained edge targets when constructing `prefix`.
    // Every member of `sibling_addresses` must therefore end up represented
    // in the final `frontiers` set (a candidate with provenance that still
    // failed every eligibility path -- precise and known_but_unemitted_
    // target alike -- for some *other* structural reason is vanishingly
    // rare given every currently-reachable such candidate shares the same
    // decode-time provenance/mapping/access shape as an ordinary precise
    // frontier); if it ever happens, `prefix` may retain a block whose edge
    // this partial program could not actually lower, so the whole promotion
    // fails closed here rather than risk emitting an unrepresentable
    // artifact.
    std::set<Address> retained_addresses;
    for (const auto &retained : frontiers)
      if (retained.diagnostic.provenance) retained_addresses.insert(retained.diagnostic.provenance->source.address.value);
    std::set<Address> retained_block_entries;
    for (const auto &block : prefix->static_blocks) retained_block_entries.insert(block.id.entry.value);
    // SEG-007-T144: under ADR-0013 Decision §7 Phase B seed expansion, two
    // independent per-seed walks (ADR-0011 §4.1) each contribute their own
    // best-effort SEG-007-T064 sibling exploration. A sibling candidate raised
    // by one walk can end up with no retained block edge targeting it at all
    // once the aggregate prefix is pruned -- a pure orphan candidate this
    // partial program neither needs to lower nor can misrepresent, because the
    // per-edge check in runtime_frontier_eligible already rejects any retained
    // block whose edge target is not a retained entry, the frontier, or a
    // still-represented sibling. Tolerating exactly that orphan case (no edge
    // in the pruned prefix targets it, and it is not itself a retained
    // frontier) lets a Phase B round emit its expanded prefix rather than
    // failing the whole translation closed, without weakening the invariant
    // for any sibling an actual retained edge still points at. The single-seed
    // path keeps the exact pre-existing strict equality, so its behavior stays
    // byte-identical.
    // SEG-007-T214 / ADR-0028 §9: an obligated target this closure represented
    // via its own newly-constructed typed frontier (`authoritative_exact_
    // closure_represented_addresses`) is an INTENTIONAL retained frontier, not
    // an inconsistency the SEG-007-T064 sibling-consistency check below should
    // ever reject -- folded into the comparison set exactly like every other
    // already-recognized sibling address.
    std::set<Address> sibling_addresses_with_closure = sibling_addresses;
    sibling_addresses_with_closure.insert(authoritative_exact_closure_represented_addresses.begin(),
                                           authoritative_exact_closure_represented_addresses.end());
    if (retained_addresses != sibling_addresses_with_closure) {
      bool tolerable = seeds.size() > 1U;
      for (const auto address : sibling_addresses_with_closure) {
        if (retained_addresses.contains(address)) continue;
        // ADR-0038 may turn a formerly non-retained candidate frontier into
        // an ordinary rooted executable entry. That is the stronger existing
        // representation and truthfully supersedes the stale frontier fact.
        if (retained_block_entries.contains(address)) continue;
        const bool edge_targets_address = std::any_of(
            prefix->static_edges.begin(), prefix->static_edges.end(),
            [&](const M68kStaticEdge &edge) { return edge.target.value == address; });
        if (edge_targets_address) { tolerable = false; break; }
      }
      for (const auto address : retained_addresses)
        if (!sibling_addresses_with_closure.contains(address)) { tolerable = false; break; }
      if (!tolerable) return reject();
    }
    // SEG-007-T214 / ADR-0028 §9.4: the final generation-time backstop
    // verifier, independent of the §9.1-§9.3 primary repair mechanism above.
    // For every retained exact direct-control edge whose source is selected
    // for native emission (a member of this final `prefix`'s own retained
    // instruction set), its target must be (1) a final retained block entry,
    // (2) the subject of an actual emitted typed frontier-stop
    // representation (a member of the final, untruncated `frontiers` set's
    // own diagnostic provenance addresses -- never merely structural
    // `candidate_frontier_addresses`/`semantic_partition_boundary_addresses`
    // membership or bare decoded presence, per §9.6), or (3) another
    // documented supported-dispatch representation (none beyond the two
    // above for this edge family). This must never actually fire once
    // §9.1-§9.3/§9.6 are correct; it exists as the backstop that fails
    // generation closed if they are not.
    {
      std::set<Address> final_retained_entries;
      for (const auto &block : prefix->static_blocks) final_retained_entries.insert(block.id.entry.value);
      std::set<Address> final_retained_instructions;
      for (const auto &block : prefix->static_blocks)
        for (const auto &instruction : block.instructions)
          final_retained_instructions.insert(instruction.source.address.value);
      std::set<Address> final_frontier_addresses;
      for (const auto &frontier : frontiers)
        if (frontier.diagnostic.provenance)
          final_frontier_addresses.insert(frontier.diagnostic.provenance->source.address.value);
      for (const auto &edge : prefix->static_edges) {
        if (edge.kind != M68kStaticEdgeKind::direct_branch && edge.kind != M68kStaticEdgeKind::direct_call) continue;
        if (edge.target.space != TargetAddressSpace::m68k_program) continue;
        if (!final_retained_instructions.contains(edge.source_instruction.source.address.value)) continue;
        if (final_retained_entries.contains(edge.target.value)) continue;
        if (final_frontier_addresses.contains(edge.target.value)) continue;
        auto r = rejected(DirectFlowDiagnostic::authoritative_exact_target_closure_exhausted, program);
        set_source(r, entry);
        return r;
      }
    }
    // SEG-007-T183 / ADR-0028 §8: `build_analysis`'s own `prefix` copy was
    // snapshotted from `analysis` before this function's retained-block/
    // ingress/frontier-projection instrumentation was written onto the
    // OUTER `analysis.offline_inventory_stitch_metrics` (`stitch_metrics`),
    // so `prefix->offline_inventory_stitch_metrics` is stale for exactly
    // those fields. Refresh it from the fully up-to-date outer copy just
    // before returning, so a caller reading the final `FrontendPartialProgram
    // ::accepted_prefix`'s own metrics (not only the normalized stderr line)
    // observes the same values. `semantic_partition_boundary_addresses`
    // itself does not need this refresh (assigned onto `analysis` before
    // `build_analysis` ever runs, so `prefix`'s own copy was already
    // current), and every other field predates T183 and was already correct.
    prefix->offline_inventory_stitch_metrics = analysis.offline_inventory_stitch_metrics;
    return FrontendPartialProgram{std::move(*prefix), std::move(frontiers)};
  };

  // discover_m68k_static_graph above already registered `entry` as the first
  // block entry and ran the whole walk; `failure` (translated above from its
  // returned primary issue, if any) is this compatibility adapter's sole
  // remaining pass/fail signal.
  if (failure) return partial_or_rejection();

  // Final block-construction pass: only after the whole walk has succeeded
  // are FrontendAnalysis::decoded/ir/static_blocks populated, from the
  // canonical decode/block-entry maps built above. Every early-return
  // failure path above returns *failure directly instead, so no caller can
  // ever observe a partial successful graph.
  auto completed = build_analysis(false, false);
  if (!completed) return rejected(DirectFlowDiagnostic::startup_graph_mismatch, program);
  if (program.synthetic_completion) {
    const auto &contract = *program.synthetic_completion;
    const auto sentinel = contract.sentinel_return_pc;
    const bool sentinel_valid = sentinel.space == TargetAddressSpace::m68k_program &&
        (sentinel.value & UINT32_C(0xFF000000)) == 0U && (sentinel.value & 1U) == 0U;
    const bool overlaps_mapping = std::any_of(program.mapping_claims.begin(), program.mapping_claims.end(),
        [&](const MappingClaim &claim) { return sentinel.value >= claim.target_begin.value && sentinel.value < claim.target_end.value; });
    const bool overlaps_instruction = std::any_of(completed->decoded.begin(), completed->decoded.end(),
        [&](const M68kDecodedInstruction &instruction) { return sentinel.value >= instruction.provenance.source.address.value &&
          sentinel.value < instruction.provenance.source.address.value + instruction.provenance.length.value; });
    const bool overlaps_entry = std::any_of(completed->static_blocks.begin(), completed->static_blocks.end(),
        [&](const M68kStaticBlock &block) { return same(sentinel, block.id.entry); });
    const bool overlaps_continuation = std::any_of(completed->static_frames.begin(), completed->static_frames.end(),
        [&](const M68kStaticFrame &frame) { return same(sentinel, frame.call.continuation); });
    const bool overlaps_routable_region = sentinel.value < UINT32_C(0x00400000) ||
        (sentinel.value >= UINT32_C(0x00A10000) && sentinel.value < UINT32_C(0x00A10020)) ||
        (sentinel.value >= UINT32_C(0x00FF0000) && sentinel.value < UINT32_C(0x01000000));
    // The synthetic terminal still executes a real RTS.  Its initial stack
    // slot must therefore be a valid longword in the same persistent work-RAM
    // window used by every other RTS; never manufacture a host-side slot.
    const bool valid_completion_slot = completed->startup_ingress &&
        (completed->startup_ingress->initial_ssp & 1U) == 0U &&
        m68k_startup_ram_range_in_range(completed->startup_ingress->initial_ssp, 4U);
    if (!completion_rts || !sentinel_valid || !valid_completion_slot || overlaps_mapping || overlaps_instruction || overlaps_entry || overlaps_continuation || overlaps_routable_region) {
      return rejected(DirectFlowDiagnostic::startup_graph_mismatch, program);
    }
    completed->completion = FrontendAnalysis::CompletionRecord{*completion_rts, sentinel};
  }
  return *completed;
}

namespace {

bool populate_immutable_rom_aot_entries(const FrontendProgram &program,
                                        FrontendAnalysis &analysis) {
  std::map<Address, FrontendAnalysis::ImmutableRomAotEntry> entries;
  // SEG-007-T246: `analysis.static_frames` is already fully populated by the
  // discovery walk that produced this `FrontendAnalysis` (or its accepted
  // prefix) before this function ever runs -- this function only ever
  // writes `analysis.immutable_rom_aot_entries` below, never
  // `static_frames`. A non-empty `static_frames` guarantees ADR-0011's
  // whole-program `runtime_return_target_set` (built from exactly these
  // frames' own `call.continuation` addresses, plus any call-shaped Tier-2
  // continuations discovered later) will also be non-empty at emission
  // time, so this is a conservative (never unsound) proxy for "the
  // generic runtime-return authority `return_from_subroutine`'s existing
  // semantic owner needs will actually be available for this program."
  const bool return_target_authority_available = !analysis.static_frames.empty();
  for (const auto &range : program.immutable_rom_aot_ranges) {
    // Iterate in a wider type: UINT32_MAX is a valid exclusive mapping end,
    // and the final aligned address below it is UINT32_MAX-1. Incrementing
    // that address in uint32_t would wrap to zero and revisit the range.
    for (std::uint64_t wide_address = range.begin_address; wide_address < range.end_address;
         wide_address += 2U) {
      const Address address = static_cast<Address>(wide_address);
      const auto owners = claims(program.mapping_claims, address);
      if (owners.size() != 1U || owners.front()->name != "raw_cartridge_rom" ||
          !structurally_valid_mapping_claim(*owners.front()))
        continue;
      const auto &claim = *owners.front();
      const std::uint64_t local = static_cast<std::uint64_t>(address) - claim.target_begin.value;
      if (local > std::numeric_limits<std::uint64_t>::max() - claim.image_begin.value) continue;
      const std::uint64_t image_offset = claim.image_begin.value + local;
      if (image_offset >= claim.image_end.value || image_offset >= program.image.bytes.size()) continue;
      const auto claim_size = claim.image_end.value - claim.image_begin.value;
      if (claim.image_begin.value > program.image.bytes.size() ||
          claim_size > program.image.bytes.size() - claim.image_begin.value)
        return false;
      const auto claim_bytes = std::span<const std::uint8_t>(program.image.bytes).subspan(
          static_cast<std::size_t>(claim.image_begin.value), static_cast<std::size_t>(claim_size));
      DecodeSource source{CpuVariant::mc68000,
                          {TargetAddressSpace::m68k_program, address},
                          MoveqImageOffset{local}};
      auto decoded_result = decode_m68k_instruction(claim_bytes, source, M68kDecodeProfile::general_startup);
      auto *decoded = std::get_if<M68kDecodedInstruction>(&decoded_result);
      if (decoded == nullptr) continue;
      decoded->provenance.source.image_offset = MoveqImageOffset{image_offset};
      const std::uint64_t length = decoded->provenance.length.value;
      if (length < 2U || length > claim.image_end.value - image_offset ||
          length > program.image.bytes.size() - image_offset)
        continue;
      // A second claim owning any byte in the decoded span makes provenance
      // ambiguous even if the first byte happened to be unique.
      bool uniquely_owned = true;
      for (const auto &other : program.mapping_claims) {
        if (&other == &claim) continue;
        const std::uint64_t decoded_end = static_cast<std::uint64_t>(address) + length;
        if (address < other.target_end.value && other.target_begin.value < decoded_end) {
          uniquely_owned = false;
          break;
        }
      }
      if (!uniquely_owned) continue;
      auto operation = lift_m68k_instruction(*decoded);
      if (!m68k_operation_is_immutable_rom_aot_safe(operation, return_target_authority_available)) continue;
      FrontendAnalysis::ImmutableRomAotEntry candidate{*decoded, operation, claim};
      const auto found = entries.find(address);
      if (found == entries.end()) {
        entries.emplace(address, std::move(candidate));
      } else if (!same_decoded(found->second.decoded, candidate.decoded) ||
                 !same_ir(found->second.operation, candidate.operation) ||
                 found->second.source_mapping.name != candidate.source_mapping.name ||
                 found->second.source_mapping.target_begin.value != candidate.source_mapping.target_begin.value ||
                 found->second.source_mapping.target_end.value != candidate.source_mapping.target_end.value ||
                 found->second.source_mapping.image_begin.value != candidate.source_mapping.image_begin.value ||
                 found->second.source_mapping.image_end.value != candidate.source_mapping.image_end.value) {
        return false;
      }
    }
  }
  analysis.immutable_rom_aot_entries.clear();
  for (auto &[address, entry] : entries) {
    (void)address;
    analysis.immutable_rom_aot_entries.push_back(std::move(entry));
  }
  return true;
}

FrontendResult analyze_startup_profile(const FrontendProgram &program) {
  if (!program.startup_ingress) return rejected(DirectFlowDiagnostic::unmapped_instruction_address, program);
  const auto entry = program.startup_ingress->entry;
  if (entry.space != TargetAddressSpace::m68k_program)
    return rejected(DirectFlowDiagnostic::valid_but_unsupported_instruction, program);
  if ((entry.value & 1U) != 0U) { auto r = rejected(DirectFlowDiagnostic::odd_instruction_address, program); set_source(r, entry); return r; }

  FrontendAnalysis analysis{};
  analysis.profile = M68kFrontendProfile::genesis_rom_startup;
  analysis.mapping_claims = program.mapping_claims;
  analysis.startup_ingress = program.startup_ingress;
  // Discover the one selected graph in control-flow order: entry block,
  // direct callee, then the statically associated continuation.  This is not
  // an arbitrary five-instruction trace: every decoded operation has a fixed
  // graph role and a five-MOVEQ image fails at the second role.
  std::optional<FrontendRejected> startup_failure;
  // decode_selected returns the shared decoded record directly: it is the
  // sole verified owner of this operation's span/extension. No second,
  // startup-only projection copies raw_bytes/extension out of it.
  const auto decode_selected = [&](M68kProgramAddress pc) -> std::optional<M68kDecodedInstruction> {
    const auto found = claims(program.mapping_claims, pc.value);
      if (found.size() != 1U) {
      auto r = rejected(found.empty() ? DirectFlowDiagnostic::unmapped_instruction_address : DirectFlowDiagnostic::conflicting_address_mapping, program);
        set_source(r, pc); std::vector<MappingClaim> matched; for (const auto *claim : found) matched.push_back(*claim); set_claims(r, std::move(matched)); startup_failure = std::move(r); return std::nullopt;
    }
    const auto &claim = *found.front();
    const auto local = static_cast<std::size_t>(pc.value - claim.target_begin.value);
    const auto claim_bytes = std::span<const std::uint8_t>(program.image.bytes).subspan(
        static_cast<std::size_t>(claim.image_begin.value), static_cast<std::size_t>(claim.image_end.value - claim.image_begin.value));
    DecodeSource source{CpuVariant::mc68000, pc, {local}};
    const auto result = decode_m68k_instruction(claim_bytes, source, M68kDecodeProfile::genesis_startup);
    if (const auto *bad = std::get_if<RejectedM68kDecode>(&result)) {
      FrontendRejected r = rejected(bad->outcome == DecodeOutcome::truncated_instruction ? DirectFlowDiagnostic::truncated_instruction :
                                   bad->outcome == DecodeOutcome::illegal_instruction ? DirectFlowDiagnostic::illegal_instruction :
                                   bad->unsupported_instruction_form ? DirectFlowDiagnostic::unsupported_instruction_form :
                                   DirectFlowDiagnostic::valid_but_unsupported_instruction, program);
      r.available_bytes = bad->available_bytes; r.requested_length = bad->requested_length;
      if (bad->has_instruction_length) r.instruction_length = bad->instruction_length;
      // A failed primary-word read has no byte provenance, but it still has a
      // verified typed source and mapped image offset.  Do not make either
      // fact conditional on whether two bytes were available.
      set_source(r, pc);
      r.image_offset = MoveqImageOffset{claim.image_begin.value + local};
      if (bad->has_provenance) { auto p = bad->provenance; p.source.image_offset.value = claim.image_begin.value + local; r.provenance = p; r.direct.provenance = p; r.direct.has_provenance = true; }
       set_claims(r, {claim}); startup_failure = std::move(r); return std::nullopt;
    }
    auto decoded = std::get<M68kDecodedInstruction>(result);
    decoded.provenance.source.image_offset.value = claim.image_begin.value + local;
    const auto length = static_cast<std::size_t>(decoded.provenance.length.value);
    if (length > claim_bytes.size() - local) { // decoder made the same check; retain fail-closed guard.
       auto r = rejected(DirectFlowDiagnostic::truncated_instruction, program); set_source(r, pc); set_claims(r, {claim}); startup_failure = std::move(r); return std::nullopt;
    }
    // Operand and direct-target policy is static: reject before an executable
    // artifact exists, using the selected instruction's verified provenance.
    // Runtime receives only this already validated trace. The alignment
    // check reuses m68k_startup_absolute_operand_alignment, the same
    // predicate execute_m68k_frontend_startup applies as its defensive
    // runtime re-check, so the two cannot drift on the rule.
     const auto reject_selected = [&](DirectFlowDiagnostic category, std::optional<std::uint32_t> target = {}) -> std::optional<M68kDecodedInstruction> {
      auto r = rejected(category, program);
      set_source(r, pc);
      r.image_offset = decoded.provenance.source.image_offset;
      r.provenance = decoded.provenance;
      r.direct.provenance = decoded.provenance;
      r.direct.has_provenance = true;
      r.instruction_length = decoded.provenance.length.value;
      r.accesses.push_back({0U, StartupBusKind::instruction_read, decoded.provenance.source.address,
                             decoded.raw_bytes, "raw_cartridge_rom", decoded.provenance});
      if (target) { r.direct.target = {{}, *target}; r.direct.has_target = true; }
       set_claims(r, {claim}); startup_failure = std::move(r);
       return std::nullopt;
    };
    if (decoded.kind == M68kInstructionKind::move) {
      if (const auto diagnostic = m68k_resolve_static_general_operand(
              program, decoded.source_ea, decoded.size, M68kMemoryAccessDirection::read, decoded.provenance))
        return reject_selected(*diagnostic, decoded.source_ea.absolute_address);
      if (const auto diagnostic = m68k_resolve_static_general_operand(
              program, decoded.destination_ea, decoded.size, M68kMemoryAccessDirection::write, decoded.provenance))
        return reject_selected(*diagnostic, decoded.destination_ea.absolute_address);
    }
    // Shared JSR target validation is owned by
    // discover_m68k_static_call_return via validate_m68k_static_call_target,
    // applied to this decoded call before the callee address is decoded.
     return decoded;
  };
  const auto graph_mismatch = [&](const M68kDecodedInstruction &decoded) -> FrontendResult {
    auto r = rejected(DirectFlowDiagnostic::startup_graph_mismatch, program);
    set_source(r, decoded.provenance.source.address);
    r.image_offset = decoded.provenance.source.image_offset;
    r.provenance = decoded.provenance;
    r.direct.provenance = decoded.provenance;
    r.direct.has_provenance = true;
    r.instruction_length = decoded.provenance.length.value;
    return r;
  };
  // decoded/ir are the sole owners of every selected operation's decode/lift
  // facts; there is no second startup-only projection of them.
  const auto append = [&](const M68kDecodedInstruction &decoded) {
    analysis.decoded.push_back(decoded);
    analysis.ir.push_back(lift_m68k_instruction(decoded));
  };

  auto first = decode_selected(entry); if (!first) return *startup_failure;
  if (first->kind != M68kInstructionKind::moveq) return graph_mismatch(*first);
  append(*first);
  const auto store_address = static_cast<std::uint32_t>(entry.value + 2U);
  auto store = decode_selected({TargetAddressSpace::m68k_program, store_address}); if (!store) return *startup_failure;
  if (store->kind != M68kInstructionKind::move || store->size != M68kMemoryAccessWidth::long_word ||
      store->source_ea.mode != M68kEaMode::data_register || store->source_ea.reg != 0U ||
      store->destination_ea.mode != M68kEaMode::absolute_long) return graph_mismatch(*store);
  append(*store);
  const auto call_address = static_cast<std::uint32_t>(store_address + 6U);
  auto call_decoded = decode_selected({TargetAddressSpace::m68k_program, call_address}); if (!call_decoded) return *startup_failure;
  if (call_decoded->kind != M68kInstructionKind::jsr || call_decoded->source_ea.mode != M68kEaMode::absolute_long)
    return graph_mismatch(*call_decoded);
  // Validate the direct call target before the callee address is ever decoded:
  // an invalid target must fail closed on the call instruction's own
  // provenance, not surface as an unrelated decode failure at a bogus PC.
  const auto call_target = m68k_canonical_ea_address(call_decoded->source_ea);
  if (const auto failure = validate_m68k_static_call_target(program, call_target))
    return build_static_call_target_rejection(program, *call_decoded, *failure, call_target);
  const M68kProgramAddress callee_target{{}, call_target};
  append(*call_decoded);
  auto ret = decode_selected(callee_target); if (!ret) return *startup_failure;
  if (ret->kind != M68kInstructionKind::rts) return graph_mismatch(*ret);
  const auto discovery = discover_m68k_static_call_return(program, *call_decoded, *ret);
  if (const auto *failure = std::get_if<FrontendRejected>(&discovery)) return *failure;
  const auto &accepted = std::get<StaticCallReturnDiscovery>(discovery);
  const auto &call = accepted.call;
  // The RTS instruction's own decoded/lifted extension is not repurposed to
  // carry the continuation address: analysis.static_frames.front().call.continuation
  // is the single already-discovered owner of that fact (T006).
  append(*ret);
  auto load = decode_selected(call.continuation); if (!load) return *startup_failure;
  if (load->kind != M68kInstructionKind::move || load->size != M68kMemoryAccessWidth::long_word ||
      load->source_ea.mode != M68kEaMode::absolute_long || load->destination_ea.mode != M68kEaMode::data_register ||
      load->destination_ea.reg != 1U) return graph_mismatch(*load);
  append(*load);

  analysis.static_blocks.push_back({BlockId{entry}, {analysis.decoded[0].provenance,
                                                        analysis.decoded[1].provenance,
                                                        analysis.decoded[2].provenance}});
  analysis.static_blocks.push_back({BlockId{call.callee}, {analysis.decoded[3].provenance}});
  analysis.static_blocks.push_back({BlockId{call.continuation}, {analysis.decoded[4].provenance}});
  analysis.static_frames.push_back(accepted.frame);
  analysis.static_edges.push_back(accepted.call_edge);
  analysis.static_edges.push_back(accepted.return_edge);
  return analysis;
}
}

FrontendResult analyze_m68k_frontend(const FrontendProgram &program) {
  // Image validation is deliberately complete before any source address is
  // resolved.  This makes claim-bounded reads the only possible byte source.
  if (program.cpu_variant != CpuVariant::mc68000) return rejected(DirectFlowDiagnostic::valid_but_unsupported_instruction, program);
  if (program.image.source_id.empty()) { auto r=rejected(DirectFlowDiagnostic::invalid_frontend_image_source_id,program); r.image_source_id.reset(); r.supplied_image_source_id=program.image.source_id; return r; }
  if (program.image.byte_length != program.image.bytes.size()) { auto r=rejected(DirectFlowDiagnostic::frontend_image_byte_length_mismatch,program); r.image_source_id.reset(); r.supplied_image_source_id=program.image.source_id; r.declared_image_byte_length=program.image.byte_length; r.actual_image_byte_length=program.image.bytes.size(); return r; }
  for(const auto &claim:program.mapping_claims) if(!valid_claim(claim,program.image.byte_length)) { auto r=rejected(DirectFlowDiagnostic::invalid_mapping_claim,program); set_claims(r,{claim}); return r; }
  if (program.profile == M68kFrontendProfile::genesis_rom_startup)
    return analyze_startup_profile(program);
  if (program.profile == M68kFrontendProfile::general_startup) {
    auto result = discover_m68k_general_startup(program);
    bool valid = true;
    if (auto *accepted = std::get_if<FrontendAnalysis>(&result))
      valid = populate_immutable_rom_aot_entries(program, *accepted);
    else if (auto *partial = std::get_if<FrontendPartialProgram>(&result))
      valid = populate_immutable_rom_aot_entries(program, partial->accepted_prefix);
    if (!valid) return rejected(DirectFlowDiagnostic::startup_graph_mismatch, program);
    return result;
  }
  std::vector<M68kProgramAddress> seeds;
  for(const auto &seed:program.analysis_entries) if(std::none_of(seeds.begin(),seeds.end(),[&](const auto &old){return same(old,seed);})) seeds.push_back(seed);
  if(seeds.empty()) return rejected(DirectFlowDiagnostic::unmapped_instruction_address,program);
  for(const auto &seed:seeds) {
    if(seed.space != TargetAddressSpace::m68k_program) return rejected(DirectFlowDiagnostic::valid_but_unsupported_instruction,program);
    if((seed.value&1U)!=0U) { auto r=rejected(DirectFlowDiagnostic::odd_instruction_address,program); set_source(r,seed); return r; }
    const auto found=claims(program.mapping_claims,seed.value);
    if(found.size()!=1U) { auto r=rejected(found.empty()?DirectFlowDiagnostic::unmapped_instruction_address:DirectFlowDiagnostic::conflicting_address_mapping,program); set_source(r,seed); std::vector<MappingClaim> matched; for(const auto *c:found) matched.push_back(*c); set_claims(r,std::move(matched)); return r; }
  }

  DirectFlowProgram direct_program{};
  direct_program.mappings=program.mapping_claims;
  direct_program.reset_entry=seeds.front();
  const auto discovered=discover_m68k_direct_flow_entries(
      std::span<const std::uint8_t>(program.image.bytes), direct_program, seeds);
  if(const auto *failure=std::get_if<RejectedDirectFlow>(&discovered)) {
    FrontendRejected r=rejected(failure->category,program);
    r.direct=*failure;
    const auto source=failure->has_provenance ? failure->provenance.source.address : failure->block.entry;
    set_source(r,source);
    r.available_bytes=failure->available_bytes;
    r.requested_length=failure->requested_length;
    if(failure->category!=DirectFlowDiagnostic::truncated_instruction && failure->has_instruction_length)
      r.instruction_length=failure->instruction_length;
    if(failure->has_provenance) { r.provenance=failure->provenance; r.image_offset=failure->provenance.source.image_offset; }
    // Lookup follows the actual failing decode PC, never the worklist block entry.
    const auto found=claims(program.mapping_claims,source.value);
    if(!found.empty()) { std::vector<MappingClaim> matched;for(const auto *claim:found)matched.push_back(*claim);set_claims(r,std::move(matched)); }
    if(!r.image_offset && found.size()==1U)
      r.image_offset=MoveqImageOffset{found.front()->image_begin.value+source.value-found.front()->target_begin.value};
    return r;
  }
  FrontendAnalysis out{}; out.mapping_claims=program.mapping_claims;
  out.direct_flow=std::get<DirectFlowAnalysis>(discovered);
  out.decoded=out.direct_flow.decoded;
  out.ir=out.direct_flow.ir;
  // Finite transitive closure: iterate a bounded adjacency matrix, never over
  // a set that is modified by the same edge scan.
  const auto n=out.direct_flow.blocks.size(); std::vector<std::vector<bool>> reach(n,std::vector<bool>(n));
  for(std::size_t i=0;i<n;++i)reach[i][i]=true;
  for(std::size_t i=0;i<n;++i)for(const auto&e:out.direct_flow.edges)if(same(e.source_block.entry,out.direct_flow.blocks[i].provenance.id.entry))for(std::size_t j=0;j<n;++j)if(same(e.target,out.direct_flow.blocks[j].provenance.id.entry))reach[i][j]=true;
  for(std::size_t k=0;k<n;++k)for(std::size_t i=0;i<n;++i)for(std::size_t j=0;j<n;++j)reach[i][j]=reach[i][j]||(reach[i][k]&&reach[k][j]);
  // SCC membership is graph-derived, but unit metadata is canonical rather
  // than worklist-derived.  In particular, do not concatenate provenance in
  // FIFO/discovery order: the sorted member order owns that ordering.
  std::vector<bool> assigned(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (assigned[i]) continue;
    StaticEmissionUnit unit{};
    for (std::size_t j = 0; j < n; ++j) {
      if (reach[i][j] && reach[j][i]) {
        assigned[j] = true;
        unit.members.push_back(out.direct_flow.blocks[j].provenance.id);
      }
    }
    std::sort(unit.members.begin(), unit.members.end(), [](const auto &a, const auto &b) {
      return less(a, b);
    });
    unit.entry_block = unit.members.front();
    for (const auto &member : unit.members) {
      const auto index = block_index(out.direct_flow, member.entry);
      if (!index) return rejected(DirectFlowDiagnostic::unmapped_instruction_address, program);
      const auto &instructions = out.direct_flow.blocks[*index].provenance.instructions;
      unit.provenance.insert(unit.provenance.end(), instructions.begin(), instructions.end());
    }
    out.units.push_back(std::move(unit));
  }
  std::sort(out.units.begin(), out.units.end(), [](const auto &a, const auto &b) {
    return less(a.members.front(), b.members.front());
  });
  for (std::size_t i = 0; i < out.units.size(); ++i) {
    out.units[i].ordinal = static_cast<std::uint32_t>(i);
    std::ostringstream symbol;
    symbol << "m68k_unit_" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << i;
    out.units[i].id = symbol.str();
  }
  return out;
}

FrontendVectorResult validate_and_execute_m68k_frontend_vector(const FrontendAnalysis &analysis,const FrontendOracleVector &v,std::string_view fixture,std::string_view hash) {
  FrontendVectorRejected bad{};bad.vector_id=v.vector_id;bad.fixture_id=v.fixture_id;bad.image_sha256=v.image_sha256;bad.cpu_variant=v.cpu_variant;bad.execution_entry=v.execution_entry;bad.expected_fixture_id=fixture;bad.expected_image_sha256=hash;bad.expected_cpu_variant=CpuVariant::mc68000;
  if(v.fixture_id!=fixture){bad.category=DirectFlowDiagnostic::vector_fixture_id_mismatch;return bad;}if(v.image_sha256!=hash){bad.category=DirectFlowDiagnostic::vector_image_sha256_mismatch;return bad;}if(v.cpu_variant!=CpuVariant::mc68000){bad.category=DirectFlowDiagnostic::vector_cpu_variant_mismatch;return bad;}if(v.execution_entry.space!=TargetAddressSpace::m68k_program){bad.category=DirectFlowDiagnostic::vector_execution_entry_space_mismatch;return bad;}if(v.execution_entry.value&1U){bad.category=DirectFlowDiagnostic::odd_instruction_address;bad.block=BlockId{v.execution_entry};return bad;}
  const auto mapped=claims(analysis.mapping_claims,v.execution_entry.value); if(mapped.empty()){bad.category=DirectFlowDiagnostic::unmapped_instruction_address;bad.block=BlockId{v.execution_entry};bad.mapping_claims=std::vector<MappingClaim>{};return bad;}std::vector<MappingClaim> mapped_claims;for(const auto *claim:mapped)mapped_claims.push_back(*claim);
  const auto index=block_index(analysis.direct_flow,v.execution_entry); if(!index){ bad.block=BlockId{v.execution_entry};bad.mapping_claims=std::move(mapped_claims); for(const auto &block:analysis.direct_flow.blocks)for(const auto&p:block.provenance.instructions)if(v.execution_entry.value>p.source.address.value&&v.execution_entry.value<p.source.address.value+p.length.value){bad.category=DirectFlowDiagnostic::execution_entry_inside_discovered_instruction;bad.provenance=p;return bad;} for(const auto &block:analysis.direct_flow.blocks)if(v.execution_entry.value>block.provenance.id.entry.value && v.execution_entry.value<block.provenance.instructions.back().source.address.value+block.provenance.instructions.back().length.value){bad.category=DirectFlowDiagnostic::execution_entry_inside_discovered_block;for(const auto&p:block.provenance.instructions)if(p.source.address.value==v.execution_entry.value)bad.provenance=p;return bad;} bad.category=DirectFlowDiagnostic::execution_entry_not_discovered_block_start;return bad; }
  const auto &block=analysis.direct_flow.blocks[*index];
  for(const auto &unit:analysis.units) if(std::any_of(unit.members.begin(),unit.members.end(),[&](const auto&m){return same(m.entry,v.execution_entry);})) {
    DirectFlowState initial=v.initial;
    // A vector's entry is authoritative; its register/SR seed must not select
    // a different discovered block accidentally.
    initial.pc=v.execution_entry;
    if(v.block_instruction_counts.empty() || std::any_of(v.block_instruction_counts.begin(),v.block_instruction_counts.end(),[](const auto count){return count==0U;})){bad.category=DirectFlowDiagnostic::vector_block_instruction_count_mismatch;return bad;}
    const auto execution=execute_m68k_direct_flow(analysis.direct_flow,initial,v.block_instruction_counts.size());
    if(const auto *failed=std::get_if<RejectedDirectFlow>(&execution)){bad.category=failed->category;return bad;}
    const auto &accepted=std::get<DirectFlowExecution>(execution);
    if(accepted.boundaries.size()!=v.block_instruction_counts.size()+1U){bad.category=DirectFlowDiagnostic::vector_block_instruction_count_mismatch;return bad;}
    for(std::size_t i=0;i<v.block_instruction_counts.size();++i)if(accepted.boundaries[i+1U].block.instructions.size()!=v.block_instruction_counts[i]){bad.category=DirectFlowDiagnostic::vector_block_instruction_count_mismatch;return bad;}
    return FrontendVectorAccepted{block.provenance, *mapped.front(), unit, accepted};
  }
  bad.category=DirectFlowDiagnostic::execution_entry_not_discovered_block_start;return bad;
}

std::string format_m68k_general_startup_result(const FrontendAnalysis &analysis) {
  std::ostringstream out;
  out << "{\"result\":\"accepted\",\"profile\":\"general_startup\",\"decoded\":"
      << analysis.decoded.size() << ",\"blocks\":" << analysis.static_blocks.size()
      << ",\"edges\":" << analysis.static_edges.size() << ",\"frames\":"
      << analysis.static_frames.size() << '}';
  return out.str();
}


#define format_m68k_frontend_result format_m68k_frontend_result_legacy
std::string format_m68k_frontend_result(const FrontendResult &result) { std::ostringstream out;if(const auto*ok=std::get_if<FrontendAnalysis>(&result)){out<<"{\"result\":\"accepted\",\"blocks\":"<<ok->direct_flow.blocks.size()<<",\"edges\":"<<ok->direct_flow.edges.size()<<",\"units\":[";for(std::size_t i=0;i<ok->units.size();++i){if(i)out<<',';out<<"{\"ordinal\":"<<ok->units[i].ordinal<<",\"id\":"<<json_string(ok->units[i].id)<<",\"members\":[";for(std::size_t j=0;j<ok->units[i].members.size();++j){if(j)out<<',';out<<json_string(hex(ok->units[i].members[j].entry.value,8));}out<<"]}";}return out<<"]}",out.str();}const auto&bad=std::get<FrontendRejected>(result);out<<"{\"result\":\"rejected\",\"category\":"<<json_string(m68k_direct_flow_diagnostic_name(bad.category))<<",\"image_source_id\":"<<(bad.image_source_id?json_string(*bad.image_source_id):"null");if(bad.profile==M68kFrontendProfile::direct_flow)return out<<'}',out.str();out<<",\"source_address\":"<<(bad.source_address?json_string(hex(bad.source_address->value,8)):"null")<<",\"image_offset\":";(bad.image_offset?out<<bad.image_offset->value:out<<"null");out<<",\"available_bytes\":";(bad.available_bytes?out<<*bad.available_bytes:out<<"null");out<<",\"requested_length\":";(bad.requested_length?out<<*bad.requested_length:out<<"null");out<<",\"instruction_length\":";(bad.instruction_length?out<<*bad.instruction_length:out<<"null");out<<",\"provenance\":";if(bad.provenance){const auto&p=*bad.provenance;out<<"{\"source_address\":"<<json_string(hex(p.source.address.value,8))<<",\"image_offset\":"<<p.source.image_offset.value<<",\"raw_bytes\":"<<json_string(hex(p.bytes[0],2).substr(2)+hex(p.bytes[1],2).substr(2))<<",\"length\":"<<p.length.value<<'}';}else out<<"null";out<<",\"mapping_claims\":";if(!bad.mapping_claims)out<<"null";else{out<<'[';for(std::size_t i=0;i<bad.mapping_claims->size();++i){if(i)out<<',';const auto&c=bad.mapping_claims->at(i);out<<"{\"name\":"<<json_string(c.name)<<",\"target_begin\":"<<json_string(hex(c.target_begin.value,8))<<",\"target_end\":"<<json_string(hex(c.target_end.value,8))<<",\"image_begin\":"<<c.image_begin.value<<",\"image_end\":"<<c.image_end.value<<'}';}out<<']';}out<<",\"accesses\":[";for(std::size_t i=0;i<bad.accesses.size();++i){if(i)out<<',';const auto&a=bad.accesses[i];out<<"{\"ordinal\":"<<a.ordinal<<",\"kind\":"<<static_cast<unsigned>(a.kind)<<",\"address\":"<<json_string(hex(a.address.value,8))<<",\"bytes\":\"";for(const auto byte:a.bytes)out<<hex(byte,2).substr(2);out<<"\",\"length\":"<<a.bytes.size()<<",\"region\":"<<json_string(a.region)<<'}';}out<<']';
  // SEG-007-T032: additive, scoped-only key. Every other category, profile,
  // or unpopulated case emits nothing here -- not even a null-valued key --
  // so existing output stays byte-for-byte unchanged.
  if(bad.profile==M68kFrontendProfile::general_startup&&bad.category==DirectFlowDiagnostic::unsupported_device_region_controller_io&&bad.controller_io_access_shape){
    const auto&shape=*bad.controller_io_access_shape;
    out<<",\"frontier_access_shape\":{\"width\":"<<json_string(m68k_memory_access_width_json_name(shape.width))
       <<",\"direction\":"<<json_string(m68k_memory_access_direction_json_name(shape.direction))<<",\"mismatches\":[";
    for(std::size_t i=0;i<shape.mismatches.size();++i){if(i)out<<',';out<<json_string(controller_io_access_shape_mismatch_json_name(shape.mismatches[i]));}
    out<<"]";
    // SEG-007-T035: additive, scoped-only key -- present if and only if
    // target_register_class is populated (i.e. only when mismatches already
    // contains different_controller_io_register_shape); absent (no key,
    // never null) otherwise, matching SEG-007-T032's own absence discipline.
    if(shape.target_register_class)
      out<<",\"target_register_class\":"<<json_string(controller_io_target_register_class_json_name(*shape.target_register_class));
    out<<"}";
  }
  return out<<'}',out.str(); }

#undef format_m68k_frontend_result
std::string format_m68k_frontend_result(const FrontendResult &result) {
  // SEG-007-T064: `frontiers` is now a JSON array, one object per retained
  // exit, in the same order FrontendPartialProgram.frontiers already holds
  // (source-address/target-address-or-zero/class ordered, per the amended
  // architecture contract's §1.3). This static CLI report is unrelated to,
  // and unchanged in shape from, the compiled bridge binary's own wire
  // report (tools/genesis_startup_bridge.py's --compare-runs schema), which
  // only ever reports the single exit the runtime actually dispatched to.
  if (const auto *partial = std::get_if<FrontendPartialProgram>(&result)) {
    std::ostringstream out;
    out << "{\"result\":\"partial\",\"frontiers\":[";
    for (std::size_t index = 0; index < partial->frontiers.size(); ++index) {
      if (index) out << ',';
      const auto &frontier = partial->frontiers[index];
      // Reuses the existing pure-C GENESIS_STOP_* enumerator name
      // (c4_stop_class) rather than inventing a second snake_case name
      // vocabulary for GenesisFrontierClass -- every one of the six values
      // has a defined mapping, so this is never null for a promoted
      // FrontendPartialProgram's own retained exit.
      const auto class_name = c4_stop_class(frontier.class_);
      out << "{\"category\":" << json_string(m68k_direct_flow_diagnostic_name(frontier.diagnostic.category))
          << ",\"class\":" << (class_name ? json_string(*class_name) : "null");
      // SEG-007-T064 Checkpoint 7 / ADR 0004
      // (docs/decisions/0004-generic-frontier-classification-privacy-boundary.md):
      // additive, scoped-only keys, present if and only if this exact class
      // populated the already-legitimately-held field each is derived from;
      // absent (no key, never null) for every other class, matching this
      // function's own existing additive-key-only-when-populated convention
      // (see the frontier_access_shape/target_register_class precedent
      // above, SEG-007-T032/T035). Neither key's value is ever the raw word
      // or address itself, only the derived classification name.
      if (frontier.class_ == GenesisFrontierClass::unsupported_cpu_form && frontier.diagnostic.provenance) {
        const auto &primary_bytes = frontier.diagnostic.provenance->bytes;
        const auto word = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(primary_bytes[0]) << 8U) | primary_bytes[1]);
        out << ",\"opcode_line\":" << json_string(m68k_opcode_line_name(word));
      }
      if ((frontier.class_ == GenesisFrontierClass::unsupported_device_access ||
           frontier.class_ == GenesisFrontierClass::unsupported_memory_region) &&
          frontier.access) {
        out << ",\"region_class\":"
            << json_string(m68k_frontier_region_class_name(frontier.access->address.value,
                                                             static_cast<std::uint32_t>(frontier.access->width),
                                                             partial->accepted_prefix.mapping_claims));
      }
      // SEG-007-T064 Checkpoint 8: restores this branch to the same field
      // set format_m68k_frontend_result_legacy's own `rejected` branch has
      // always carried unconditionally for a plain FrontendRejected (see
      // that function, a few lines above) -- source_address/image_offset/
      // provenance/mapping_claims, plus access_address/width/direction for
      // the two access-bearing classes. This is not a new privacy
      // exception: `frontier.diagnostic` is itself a FrontendRejected, and
      // this project's own CLI has always unconditionally reported this
      // exact same detail for a plain (non-partial) rejection in every
      // prior real-ROM validation task in this milestone; Checkpoint 7's
      // own initial `partial`-branch shape was inadvertently more
      // restrictive than that established precedent, not deliberately so.
      out << ",\"source_address\":"
          << (frontier.diagnostic.source_address ? json_string(hex(frontier.diagnostic.source_address->value, 8)) : "null")
          << ",\"image_offset\":";
      if (frontier.diagnostic.image_offset) out << frontier.diagnostic.image_offset->value; else out << "null";
      out << ",\"provenance\":";
      if (frontier.diagnostic.provenance) {
        const auto &p = *frontier.diagnostic.provenance;
        out << "{\"source_address\":" << json_string(hex(p.source.address.value, 8))
            << ",\"image_offset\":" << p.source.image_offset.value << ",\"raw_bytes\":"
            << json_string(hex(p.bytes[0], 2).substr(2) + hex(p.bytes[1], 2).substr(2))
            << ",\"length\":" << p.length.value << '}';
      } else {
        out << "null";
      }
      out << ",\"mapping_claims\":";
      if (!frontier.diagnostic.mapping_claims) {
        out << "null";
      } else {
        out << '[';
        for (std::size_t claim_index = 0; claim_index < frontier.diagnostic.mapping_claims->size(); ++claim_index) {
          if (claim_index) out << ',';
          const auto &claim = frontier.diagnostic.mapping_claims->at(claim_index);
          out << "{\"name\":" << json_string(claim.name)
              << ",\"target_begin\":" << json_string(hex(claim.target_begin.value, 8))
              << ",\"target_end\":" << json_string(hex(claim.target_end.value, 8))
              << ",\"image_begin\":" << claim.image_begin.value << ",\"image_end\":" << claim.image_end.value << '}';
        }
        out << ']';
      }
      if (frontier.access) {
        out << ",\"access_address\":" << json_string(hex(frontier.access->address.value, 8))
            << ",\"access_width\":" << json_string(m68k_memory_access_width_json_name(frontier.access->width))
            << ",\"access_direction\":" << json_string(m68k_memory_access_direction_json_name(frontier.access->direction));
      }
      out << '}';
    }
    out << "],\"completed_blocks\":" << partial->accepted_prefix.static_blocks.size() << '}';
    return out.str();
  }
  return format_m68k_frontend_result_legacy(result);
}


} // namespace segarecomp
