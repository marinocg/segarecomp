#include "segarecomp/cpu/m68k/direct_flow.hpp"
#include "segarecomp/cpu/m68k/decode.hpp"
#include "segarecomp/cpu/m68k/effects.hpp"
#include "segarecomp/cpu/m68k/ir.hpp"

#include <algorithm>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace segarecomp {
namespace {
using Address = std::uint32_t;

std::string hex(std::uint64_t value, unsigned width) {
  std::ostringstream output;
  output << "0x" << std::uppercase << std::hex << std::setw(static_cast<int>(width))
         << std::setfill('0') << value;
  return output.str();
}

std::string address_json(const M68kProgramAddress &address) {
  return "{\"space\":\"m68k_program\",\"value\":\"" + hex(address.value, 8) + "\"}";
}

bool contains(const MappingClaim &mapping, Address address) {
  return address >= mapping.target_begin.value && address < mapping.target_end.value;
}

std::vector<const MappingClaim *> claims(const DirectFlowProgram &program, Address address) {
  std::vector<const MappingClaim *> result;
  for (const auto &mapping : program.mappings) {
    if (contains(mapping, address)) result.push_back(&mapping);
  }
  return result;
}

bool mapping_is_valid(const MappingClaim &mapping, std::uint64_t image_size) {
  if (mapping.target_begin.value >= mapping.target_end.value ||
      mapping.image_begin.value >= mapping.image_end.value ||
      mapping.image_begin.value > image_size || mapping.image_end.value > image_size) {
    return false;
  }
  const auto target_length = static_cast<std::uint64_t>(mapping.target_end.value) -
                             mapping.target_begin.value;
  const auto image_length = mapping.image_end.value - mapping.image_begin.value;
  return target_length == image_length;
}

RejectedDirectFlow reject(DirectFlowDiagnostic category, Address entry,
                          const InstructionProvenance *provenance = nullptr) {
  RejectedDirectFlow result{};
  result.category = category;
  result.block.entry = {TargetAddressSpace::m68k_program, entry};
  if (provenance != nullptr) {
    result.provenance = *provenance;
    result.has_provenance = true;
  }
  return result;
}

const MappingClaim *mapping_for(const DirectFlowProgram &program, Address address,
                                RejectedDirectFlow &failure) {
  const auto found = claims(program, address);
  if (found.size() > 1U) {
    failure.category = DirectFlowDiagnostic::conflicting_address_mapping;
    for (const auto *mapping : found) failure.mapping_claims.push_back(*mapping);
    return nullptr;
  }
  if (found.empty()) {
    failure.category = DirectFlowDiagnostic::unmapped_instruction_address;
    return nullptr;
  }
  return found[0];
}

bool check_target(const DirectFlowProgram &program, Address target, Address entry,
                  const InstructionProvenance &provenance, RejectedDirectFlow &failure) {
  failure = reject(DirectFlowDiagnostic::odd_direct_target, entry, &provenance);
  failure.target = {TargetAddressSpace::m68k_program, target};
  failure.has_target = true;
  if ((target & 1U) != 0U) return false;
  const auto found = claims(program, target);
  if (found.size() > 1U) {
    failure.category = DirectFlowDiagnostic::conflicting_address_mapping;
    for (const auto *mapping : found) failure.mapping_claims.push_back(*mapping);
    return false;
  }
  if (found.empty()) {
    failure.category = DirectFlowDiagnostic::unmapped_direct_target;
    return false;
  }
  for (const auto &interval : program.structural_intervals) {
    const auto begin = interval.source.address.value;
    const auto end = static_cast<std::uint64_t>(begin) + interval.length.value;
    if (target > begin && static_cast<std::uint64_t>(target) < end) {
      failure.category = DirectFlowDiagnostic::mid_instruction_direct_target;
      return false;
    }
  }
  return true;
}

DirectInstruction decode(std::span<const std::uint8_t> image, const DirectFlowProgram &program,
                          Address pc, RejectedDirectFlow &failure, bool &ok,
                          M68kDecodedInstruction &shared_instruction) {
  failure = reject(DirectFlowDiagnostic::odd_instruction_address, pc);
  ok = false;
  if ((pc & 1U) != 0U) return {};
  const auto *mapping = mapping_for(program, pc, failure);
  if (mapping == nullptr) return {};
  const auto offset = mapping->image_begin.value +
                      static_cast<std::uint64_t>(pc - mapping->target_begin.value);
  const DecodeSource source{CpuVariant::mc68000, {TargetAddressSpace::m68k_program, pc}, {offset}};
  // The decoder receives this claim only, not the image prefix.  Its local
  // offset is projected back to the original image offset in every outcome.
  const auto local_offset = static_cast<std::size_t>(pc - mapping->target_begin.value);
  const DecodeSource local_source{source.cpu_variant, source.address, {local_offset}};
  const auto mapped_image = image.subspan(static_cast<std::size_t>(mapping->image_begin.value),
                                          static_cast<std::size_t>(mapping->image_end.value - mapping->image_begin.value));
  auto decoded = decode_m68k_instruction(mapped_image, local_source, M68kDecodeProfile::direct_flow);
  if (const auto *rejected = std::get_if<RejectedM68kDecode>(&decoded)) {
    failure.available_bytes = rejected->available_bytes;
    failure.requested_length = rejected->requested_length;
    failure.instruction_length = rejected->instruction_length;
    failure.has_instruction_length = rejected->has_instruction_length;
    if (rejected->has_provenance) {
      failure.provenance = rejected->provenance;
      failure.provenance.source.image_offset = source.image_offset;
      failure.has_provenance = true;
    }
    switch (rejected->outcome) {
    case DecodeOutcome::truncated_instruction:
      failure.category = DirectFlowDiagnostic::truncated_instruction;
      break;
    case DecodeOutcome::illegal_instruction:
      failure.category = DirectFlowDiagnostic::illegal_instruction;
      break;
    default:
      failure.category = DirectFlowDiagnostic::valid_but_unsupported_instruction;
      break;
    }
    return {};
  }
  auto shared = std::get<M68kDecodedInstruction>(decoded);
  shared.provenance.source.image_offset = source.image_offset;
  shared_instruction = shared;
  DirectInstruction instruction{shared.provenance};
  switch (shared.kind) {
  case M68kInstructionKind::moveq: instruction.kind = DirectFlowKind::moveq_d0; break;
  case M68kInstructionKind::subq_l_1_d0: instruction.kind = DirectFlowKind::subq_l_1_d0; break;
  case M68kInstructionKind::bne_short: instruction.kind = DirectFlowKind::bne_short; break;
  case M68kInstructionKind::bra_short: instruction.kind = DirectFlowKind::bra_short; break;
  case M68kInstructionKind::rts:
  case M68kInstructionKind::rte:
  case M68kInstructionKind::tst:
  case M68kInstructionKind::cmp:
  case M68kInstructionKind::cmpi:
  case M68kInstructionKind::cmpa:
  case M68kInstructionKind::add:
  case M68kInstructionKind::adda:
  case M68kInstructionKind::addi:
  case M68kInstructionKind::addq:
  case M68kInstructionKind::sub:
  case M68kInstructionKind::suba:
  case M68kInstructionKind::subi:
  case M68kInstructionKind::subq:
  case M68kInstructionKind::logical_and:
  case M68kInstructionKind::andi:
  case M68kInstructionKind::logical_or:
  case M68kInstructionKind::ori:
  case M68kInstructionKind::eor:
  case M68kInstructionKind::eori:
  case M68kInstructionKind::move:
  case M68kInstructionKind::movea:
  case M68kInstructionKind::clr:
  case M68kInstructionKind::not_operand:
  case M68kInstructionKind::negate_word:
  case M68kInstructionKind::negate_extended:
  case M68kInstructionKind::add_extended:
  case M68kInstructionKind::subtract_extended:
  case M68kInstructionKind::compare_memory:
  case M68kInstructionKind::add_decimal:
  case M68kInstructionKind::subtract_decimal:
  case M68kInstructionKind::negate_decimal:
  case M68kInstructionKind::exchange_registers:
  case M68kInstructionKind::movep:
  case M68kInstructionKind::set_conditional:
  case M68kInstructionKind::test_and_set:
  case M68kInstructionKind::lea:
  case M68kInstructionKind::jmp:
  case M68kInstructionKind::jsr:
  case M68kInstructionKind::swap:
  case M68kInstructionKind::ext_w:
  case M68kInstructionKind::ext_l:
  case M68kInstructionKind::pea:
  case M68kInstructionKind::link:
  case M68kInstructionKind::unlk:
  case M68kInstructionKind::btst:
  case M68kInstructionKind::bchg:
  case M68kInstructionKind::bclr:
  case M68kInstructionKind::bset:
  case M68kInstructionKind::branch:
  case M68kInstructionKind::bsr:
  case M68kInstructionKind::dbcc:
  case M68kInstructionKind::movem:
  case M68kInstructionKind::shift_rotate:
  case M68kInstructionKind::move_an_to_usp:
  case M68kInstructionKind::move_usp_to_an:
  case M68kInstructionKind::logical_immediate_to_ccr:
  case M68kInstructionKind::logical_immediate_to_sr:
  case M68kInstructionKind::move_to_sr:
  case M68kInstructionKind::nop:
  case M68kInstructionKind::move_from_sr:
  case M68kInstructionKind::move_to_ccr:
  case M68kInstructionKind::multiply_signed_word:
  case M68kInstructionKind::multiply_unsigned_word:
  case M68kInstructionKind::divide_signed_word:
  case M68kInstructionKind::divide_unsigned_word:
    // Not part of the DirectFlowKind CFG model (SEG-003); unreachable via
    // decode_m68k_instruction(..., M68kDecodeProfile::direct_flow), which
    // never selects any of these kinds (TST.L is selected only by the
    // shared M68kDecodeProfile::genesis_startup decode per SEG-007-T008; the
    // SEG-007-T023/T025 general whitelist kinds are selected only by
    // M68kDecodeProfile::general_startup).
    return {};
  }
  instruction.operand = shared.operand;
  ok = true;
  return instruction;
}

DirectFlowIrOperation project_direct_operation(const M68kIrOperation &lifted) {
  DirectFlowIrOperation result{lifted.provenance, DirectFlowIrKind::write_moveq_d0, lifted.operand};
  switch (lifted.kind) {
  case M68kIrKind::write_moveq: break;
  case M68kIrKind::subtract_quick_long_d0: result.kind = DirectFlowIrKind::subtract_quick_long_d0; break;
  case M68kIrKind::branch_ne_short: result.kind = DirectFlowIrKind::branch_ne_short; break;
  case M68kIrKind::branch_always_short: result.kind = DirectFlowIrKind::branch_always_short; break;
  case M68kIrKind::return_from_subroutine:
  case M68kIrKind::return_from_exception:
  case M68kIrKind::write_user_stack_pointer:
  case M68kIrKind::read_user_stack_pointer:
  case M68kIrKind::logical_immediate_to_ccr:
  case M68kIrKind::logical_immediate_to_sr:
  case M68kIrKind::test_operand:
  case M68kIrKind::compare:
  case M68kIrKind::compare_immediate:
  case M68kIrKind::compare_address:
  case M68kIrKind::add:
  case M68kIrKind::add_address:
  case M68kIrKind::add_immediate:
  case M68kIrKind::add_quick:
  case M68kIrKind::subtract:
  case M68kIrKind::subtract_address:
  case M68kIrKind::subtract_immediate:
  case M68kIrKind::subtract_quick:
  case M68kIrKind::logical_and:
  case M68kIrKind::logical_and_immediate:
  case M68kIrKind::logical_or:
  case M68kIrKind::logical_or_immediate:
  case M68kIrKind::exclusive_or:
  case M68kIrKind::exclusive_or_immediate:
  case M68kIrKind::write_move:
  case M68kIrKind::write_movea:
  case M68kIrKind::write_clr:
  case M68kIrKind::logical_not:
  case M68kIrKind::negate_word:
  case M68kIrKind::negate_extended:
  case M68kIrKind::add_extended:
  case M68kIrKind::subtract_extended:
  case M68kIrKind::compare_memory:
  case M68kIrKind::add_decimal:
  case M68kIrKind::subtract_decimal:
  case M68kIrKind::negate_decimal:
  case M68kIrKind::exchange_registers:
  case M68kIrKind::movep_transfer:
  case M68kIrKind::set_conditional:
  case M68kIrKind::test_and_set:
  case M68kIrKind::load_effective_address:
  case M68kIrKind::jump_general:
  case M68kIrKind::call_general:
  case M68kIrKind::write_swap:
  case M68kIrKind::sign_extend_word:
  case M68kIrKind::sign_extend_long:
  case M68kIrKind::push_effective_address:
  case M68kIrKind::link_frame:
  case M68kIrKind::unlink_frame:
  case M68kIrKind::bit_test:
  case M68kIrKind::bit_change:
  case M68kIrKind::bit_clear:
  case M68kIrKind::bit_set:
  case M68kIrKind::general_branch:
  case M68kIrKind::bsr_call:
  case M68kIrKind::dbcc_loop:
  case M68kIrKind::movem_transfer:
  case M68kIrKind::shift_rotate_register:
  case M68kIrKind::shift_rotate_memory:
  case M68kIrKind::write_status_register:
  case M68kIrKind::no_operation:
  case M68kIrKind::read_status_register:
  case M68kIrKind::write_condition_codes:
  case M68kIrKind::multiply_signed_word:
  case M68kIrKind::multiply_unsigned_word:
  case M68kIrKind::divide_signed_word:
  case M68kIrKind::divide_unsigned_word:
    break;
  }
  return result;
}

M68kIrOperation shared_operation(const DirectFlowIrOperation &operation) {
  M68kIrKind kind{M68kIrKind::write_moveq};
  switch (operation.kind) {
  case DirectFlowIrKind::write_moveq_d0: break;
  case DirectFlowIrKind::subtract_quick_long_d0: kind = M68kIrKind::subtract_quick_long_d0; break;
  case DirectFlowIrKind::branch_ne_short: kind = M68kIrKind::branch_ne_short; break;
  case DirectFlowIrKind::branch_always_short: kind = M68kIrKind::branch_always_short; break;
  }
  return {operation.provenance, kind, DataRegister::d0, operation.operand, {}, {}};
}

bool transfer(const DirectFlowIrOperation &operation) {
  return m68k_ir_is_transfer(shared_operation(operation));
}

const DirectEdge *choose(const DirectFlowAnalysis &analysis, const DirectBlock &block,
                         std::uint16_t sr) {
  for (const auto &edge : analysis.edges) {
    if (edge.source_block.entry.space == block.provenance.id.entry.space &&
        edge.source_block.entry.value == block.provenance.id.entry.value &&
        (edge.condition == DirectCondition::always ||
         ((edge.condition == DirectCondition::z_clear) == ((sr & 4U) == 0U)))) return &edge;
  }
  return nullptr;
}

void execute_op(const DirectFlowIrOperation &operation, DirectFlowState &state) {
  switch (operation.kind) {
  case DirectFlowIrKind::write_moveq_d0: {
    const auto effect = m68k_operation_effect(shared_operation(operation));
    state.d[static_cast<unsigned>(effect.register_write->reg)] = effect.register_write->value;
    state.sr = m68k_move_result_ccr(state.sr, effect.register_write->value);
    break;
  }
  case DirectFlowIrKind::subtract_quick_long_d0: {
    const auto old = state.d[0];
    state.d[0] = old - 1U;
    const auto flags = (state.d[0] == 0U ? 4U : 0U) |
                       ((state.d[0] & 0x80000000U) != 0U ? 8U : 0U) |
                       (old == 0x80000000U ? 2U : 0U) |
                       (old == 0U ? 0x11U : 0U);
    state.sr = static_cast<std::uint16_t>((state.sr & 0xFFE0U) | flags);
    break;
  }
  case DirectFlowIrKind::branch_ne_short:
  case DirectFlowIrKind::branch_always_short: break;
  }
}

const DirectBlock *block_at(const DirectFlowAnalysis &analysis, const M68kProgramAddress &pc) {
  const auto found = std::find_if(analysis.blocks.begin(), analysis.blocks.end(), [&](const auto &block) {
    return block.provenance.id.entry.space == pc.space && block.provenance.id.entry.value == pc.value;
  });
  return found == analysis.blocks.end() ? nullptr : &*found;
}
} // namespace

const char *m68k_direct_flow_diagnostic_name(DirectFlowDiagnostic diagnostic) noexcept {
  switch (diagnostic) {
  case DirectFlowDiagnostic::odd_instruction_address: return "odd_instruction_address";
  case DirectFlowDiagnostic::unmapped_instruction_address: return "unmapped_instruction_address";
  case DirectFlowDiagnostic::truncated_instruction: return "truncated_instruction";
  case DirectFlowDiagnostic::illegal_instruction: return "illegal_instruction";
  case DirectFlowDiagnostic::valid_but_unsupported_instruction: return "valid_but_unsupported_instruction";
  case DirectFlowDiagnostic::unsupported_instruction_form: return "unsupported_instruction_form";
  case DirectFlowDiagnostic::odd_direct_target: return "odd_direct_target";
  case DirectFlowDiagnostic::conflicting_address_mapping: return "conflicting_address_mapping";
  case DirectFlowDiagnostic::invalid_address_mapping: return "invalid_address_mapping";
  case DirectFlowDiagnostic::unmapped_direct_target: return "unmapped_direct_target";
  case DirectFlowDiagnostic::mid_instruction_direct_target: return "mid_instruction_direct_target";
  case DirectFlowDiagnostic::reached_unresolved_direct_edge: return "reached_unresolved_direct_edge";
  case DirectFlowDiagnostic::invalid_frontend_image_source_id: return "invalid_frontend_image_source_id";
  case DirectFlowDiagnostic::frontend_image_byte_length_mismatch: return "frontend_image_byte_length_mismatch";
  case DirectFlowDiagnostic::invalid_mapping_claim: return "invalid_mapping_claim";
  case DirectFlowDiagnostic::vector_fixture_id_mismatch: return "vector_fixture_id_mismatch";
  case DirectFlowDiagnostic::vector_image_sha256_mismatch: return "vector_image_sha256_mismatch";
  case DirectFlowDiagnostic::vector_cpu_variant_mismatch: return "vector_cpu_variant_mismatch";
  case DirectFlowDiagnostic::vector_execution_entry_space_mismatch: return "vector_execution_entry_space_mismatch";
  case DirectFlowDiagnostic::execution_entry_inside_discovered_instruction: return "execution_entry_inside_discovered_instruction";
  case DirectFlowDiagnostic::execution_entry_inside_discovered_block: return "execution_entry_inside_discovered_block";
  case DirectFlowDiagnostic::execution_entry_not_discovered_block_start: return "execution_entry_not_discovered_block_start";
  case DirectFlowDiagnostic::vector_block_instruction_count_mismatch: return "vector_block_instruction_count_mismatch";
  case DirectFlowDiagnostic::effective_address_not_24bit: return "effective_address_not_24bit";
  case DirectFlowDiagnostic::odd_effective_address: return "odd_effective_address";
  case DirectFlowDiagnostic::rom_write_prohibited: return "rom_write_prohibited";
  case DirectFlowDiagnostic::unmapped_data_access: return "unmapped_data_access";
  case DirectFlowDiagnostic::unsupported_device_region_controller_io: return "unsupported_device_region_controller_io";
  case DirectFlowDiagnostic::invalid_stack_alignment: return "invalid_stack_alignment";
  case DirectFlowDiagnostic::invalid_stack_range: return "invalid_stack_range";
  case DirectFlowDiagnostic::return_context_missing: return "return_context_missing";
  case DirectFlowDiagnostic::return_target_mismatch: return "return_target_mismatch";
  case DirectFlowDiagnostic::startup_graph_mismatch: return "startup_graph_mismatch";
  case DirectFlowDiagnostic::discovery_budget_exhausted: return "discovery_budget_exhausted";
  case DirectFlowDiagnostic::authoritative_exact_target_closure_exhausted:
    return "authoritative_exact_target_closure_exhausted";
  }
  return "unknown";
}

DirectFlowAnalysisResult discover_m68k_direct_flow_entries(
    std::span<const std::uint8_t> image, const DirectFlowProgram &program,
    std::span<const M68kProgramAddress> supplied_entries) {
  // This retained source-address check precedes mapping validation without
  // touching image bytes, preserving the inherited decode precedence.
  if (supplied_entries.empty()) return reject(DirectFlowDiagnostic::unmapped_instruction_address, program.reset_entry.value);
  for (const auto &entry : supplied_entries) {
    if ((entry.value & 1U) != 0U)
      return reject(DirectFlowDiagnostic::odd_instruction_address, entry.value);
  }
  const auto image_size = static_cast<std::uint64_t>(image.size());
  for (const auto &mapping : program.mappings) {
    if (!mapping_is_valid(mapping, image_size)) {
      auto failure = reject(DirectFlowDiagnostic::invalid_address_mapping, program.reset_entry.value);
      failure.mapping_claims.push_back(mapping);
      return failure;
    }
  }
  std::map<Address, DirectInstruction> decoded;
  std::map<Address, M68kDecodedInstruction> shared_decoded;
  DirectFlowAnalysis analysis{};
  std::set<Address> entries;
  std::vector<Address> fifo;
  for (const auto &entry : supplied_entries) {
    if (entries.insert(entry.value).second) fifo.push_back(entry.value);
  }
  for (std::size_t next = 0; next < fifo.size(); ++next) {
    const auto entry = fifo[next];
    for (Address pc = entry; !decoded.contains(pc); pc += 2U) {
      RejectedDirectFlow failure{};
      bool ok{};
      M68kDecodedInstruction shared_instruction{};
      const auto instruction = decode(image, program, pc, failure, ok, shared_instruction);
      if (!ok) return failure;
      decoded.emplace(pc, instruction);
      shared_decoded.emplace(pc, shared_instruction);
      // Retain the shared decode/lift sequence in FIFO discovery order.  CFG
      // blocks and emission metadata are canonicalized separately below.
      analysis.decoded.push_back(shared_instruction);
      analysis.ir.push_back(lift_m68k_instruction(shared_instruction));
      if (instruction.kind != DirectFlowKind::bne_short && instruction.kind != DirectFlowKind::bra_short) {
        if (pc > UINT32_MAX - 2U) return reject(DirectFlowDiagnostic::unmapped_instruction_address, entry);
        if (entries.contains(pc + 2U) && pc + 2U != entry) break;
        continue;
      }
      const auto target = static_cast<Address>(static_cast<std::int64_t>(pc) + 2 + instruction.operand);
      if (instruction.kind == DirectFlowKind::bne_short) {
        const auto fallthrough = static_cast<Address>(pc + 2U);
        if (!check_target(program, fallthrough, entry, instruction.provenance, failure)) return failure;
        if (entries.insert(fallthrough).second) fifo.push_back(fallthrough);
      }
      if (!check_target(program, target, entry, instruction.provenance, failure)) return failure;
      if (entries.insert(target).second) fifo.push_back(target);
      break;
    }
  }

  for (const auto entry : entries) {
    DirectBlock block{};
    block.provenance.id.entry = {TargetAddressSpace::m68k_program, entry};
    for (Address pc = entry; decoded.contains(pc); pc += 2U) {
      const auto &instruction = decoded.at(pc);
      const auto &shared_instruction = shared_decoded.at(pc);
      if (block.operations.empty()) block.provenance.entry_instruction = instruction.provenance;
      block.provenance.instructions.push_back(instruction.provenance);
      const auto shared_operation = lift_m68k_instruction(shared_instruction);
      const auto operation = project_direct_operation(shared_operation);
      block.operations.push_back(operation);
      if (transfer(operation) || (pc + 2U != entry && entries.contains(pc + 2U))) break;
    }
    if (block.operations.empty()) return reject(DirectFlowDiagnostic::unmapped_instruction_address, entry);
    analysis.blocks.push_back(std::move(block));
  }
  for (const auto &block : analysis.blocks) {
    const auto &last = block.operations.back();
    const auto source = last.provenance.source.address.value;
    const auto add = [&](DirectEdgeKind kind, DirectCondition condition, Address target) {
      DirectEdge edge{block.provenance.id, last.provenance, kind, condition,
                      {TargetAddressSpace::m68k_program, target}, false, {}};
      for (const auto &unresolved : program.unresolved_targets) {
        if (unresolved.space == edge.target.space && unresolved.value == edge.target.value) {
          edge.unresolved = true;
          edge.unresolved_reason = "persisted_unresolved";
        }
      }
      analysis.edges.push_back(std::move(edge));
    };
    if (last.kind == DirectFlowIrKind::branch_ne_short) {
      add(DirectEdgeKind::bne_fallthrough, DirectCondition::z_set, source + 2U);
      add(DirectEdgeKind::bne_taken, DirectCondition::z_clear,
          static_cast<Address>(static_cast<std::int64_t>(source) + 2 + last.operand));
    } else if (last.kind == DirectFlowIrKind::branch_always_short) {
      add(DirectEdgeKind::bra_taken, DirectCondition::always,
          static_cast<Address>(static_cast<std::int64_t>(source) + 2 + last.operand));
    } else {
      add(DirectEdgeKind::fallthrough, DirectCondition::always, source + last.provenance.length.value);
    }
  }
  std::sort(analysis.blocks.begin(), analysis.blocks.end(), [](const auto &left, const auto &right) {
    return left.provenance.id.entry.value < right.provenance.id.entry.value;
  });
  return analysis;
}

DirectFlowAnalysisResult discover_m68k_direct_flow(std::span<const std::uint8_t> image,
                                                     const DirectFlowProgram &program) {
  return discover_m68k_direct_flow_entries(image, program, std::span{&program.reset_entry, 1U});
}

DirectFlowExecutionResult execute_m68k_direct_flow(const DirectFlowAnalysis &analysis, DirectFlowState state,
                                                    std::uint64_t budget) {
  if (budget == 0U) return reject(DirectFlowDiagnostic::valid_but_unsupported_instruction, state.pc.value);
  const auto *initial_block = block_at(analysis, state.pc);
  if (initial_block == nullptr) return reject(DirectFlowDiagnostic::unmapped_direct_target, state.pc.value);
  DirectFlowExecution output{};
  output.boundaries.push_back({0, initial_block->provenance, std::nullopt, true, state, std::nullopt,
                               0, budget, "continue"});
  std::optional<DirectEdge> incoming;
  bool incoming_is_reset_seed = true;
  for (std::uint64_t done = 0;;) {
    const auto *block = block_at(analysis, state.pc);
    if (block == nullptr) {
      auto failure = reject(DirectFlowDiagnostic::unmapped_direct_target, state.pc.value);
      failure.target = state.pc;
      failure.has_target = true;
      return failure;
    }
    for (const auto &operation : block->operations) execute_op(operation, state);
    const auto *edge = choose(analysis, *block, state.sr);
    if (edge == nullptr) return reject(DirectFlowDiagnostic::unmapped_direct_target, block->provenance.id.entry.value);
    if (edge->unresolved) {
      auto failure = reject(DirectFlowDiagnostic::reached_unresolved_direct_edge,
                            block->provenance.id.entry.value, &edge->source_instruction);
      failure.target = edge->target;
      failure.has_target = true;
      failure.unresolved_reason = edge->unresolved_reason;
      return failure;
    }
    state.pc = edge->target;
    ++done;
    output.boundaries.push_back({done, block->provenance, incoming, incoming_is_reset_seed, state, *edge,
                                 done, budget, done == budget ? "instruction_budget_exhausted" : "continue"});
    incoming = *edge;
    incoming_is_reset_seed = false;
    if (done == budget) {
      output.final_state = state;
      output.stop_reason = "instruction_budget_exhausted";
      return output;
    }
  }
}

std::string format_m68k_direct_flow_rejection(const RejectedDirectFlow &rejection) {
  std::ostringstream output;
  output << "{\"category\":\"" << m68k_direct_flow_diagnostic_name(rejection.category)
         << "\",\"cpu_variant\":\"mc68000\",\"block_entry\":\"" << hex(rejection.block.entry.value, 8)
         << "\",\"source_address\":";
  if (rejection.has_provenance) output << address_json(rejection.provenance.source.address); else output << "null";
  output << ",\"image_offset\":";
  if (rejection.has_provenance) output << rejection.provenance.source.image_offset.value; else output << "null";
  output << ",\"available_bytes\":" << rejection.available_bytes << ",\"requested_length\":"
         << rejection.requested_length << ",\"instruction_length\":";
  if (rejection.has_instruction_length) output << rejection.instruction_length; else output << "null";
  output << ",\"raw_bytes\":";
  if (rejection.has_provenance) {
    output << '"';
    for (const auto byte : rejection.provenance.bytes) output << std::uppercase << std::hex << std::setw(2)
                                                             << std::setfill('0') << static_cast<unsigned>(byte);
    output << '"';
  } else output << "null";
  output << ",\"target\":";
  if (rejection.has_target) output << address_json(rejection.target); else output << "null";
  output << ",\"edge\":" << (rejection.has_target ? "true" : "null") << ",\"mapping_claims\":[";
  for (std::size_t index = 0; index < rejection.mapping_claims.size(); ++index) {
    const auto &mapping = rejection.mapping_claims[index];
    if (index != 0U) output << ',';
    output << "{\"name\":\"" << mapping.name << "\",\"target_span\":[\""
           << hex(mapping.target_begin.value, 8) << "\",\"" << hex(mapping.target_end.value, 8)
           << "\"],\"image_offset_span\":[" << std::dec << mapping.image_begin.value << ','
           << mapping.image_end.value << "]}";
  }
  output << "],\"unresolved_reason\":";
  if (rejection.unresolved_reason.empty()) output << "null"; else output << '"' << rejection.unresolved_reason << '"';
  return output << '}', output.str();
}

} // namespace segarecomp
