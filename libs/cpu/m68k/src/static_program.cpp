#include "segarecomp/cpu/m68k/static_program.hpp"

#include <set>
#include <tuple>

namespace segarecomp {
namespace {
using Address = std::uint32_t;

// SEG-007-T151: moved verbatim (structure and successor set) from
// `StaticGraphWalk`'s own file-local `m68k_return_reachability_successors`
// (static_discovery.cpp) so both the per-seed walk and the new cross-seed
// aggregate trace below share exactly one successor rule -- never two
// independently maintained copies of "which addresses execution may reach
// from here while remaining within the same subroutine body". An RTS is
// this trace's own terminal and contributes no successor; a JSR/BSR
// contributes only its own continuation (its callee is a distinct
// subroutine boundary, traced independently for that nested call's own
// frame(s), never folded into this trace).
std::vector<Address> return_reachability_successors(
    Address addr, const M68kDecodedInstruction &decoded,
    const std::map<Address, M68kIndirectTargetEaSet> &accepted_tier1_by_source) {
  std::vector<Address> out;
  const auto next_pc = static_cast<Address>(addr + decoded.provenance.length.value);
  switch (decoded.kind) {
  case M68kInstructionKind::bne_short:
    out.push_back(next_pc);
    out.push_back(m68k_branch_target(addr, decoded.operand));
    break;
  case M68kInstructionKind::bra_short:
    out.push_back(m68k_branch_target(addr, decoded.operand));
    break;
  case M68kInstructionKind::branch: {
    const auto raw = decoded.source_ea.immediate_value;
    const auto signed_disp = decoded.size == M68kMemoryAccessWidth::byte
                                  ? static_cast<std::int32_t>(static_cast<std::int8_t>(raw))
                                  : static_cast<std::int32_t>(static_cast<std::int16_t>(raw));
    out.push_back(m68k_branch_target(addr, signed_disp));
    if (decoded.condition != M68kCondition::always) out.push_back(next_pc);
    break;
  }
  case M68kInstructionKind::dbcc: {
    const auto raw = decoded.source_ea.immediate_value;
    const auto signed_disp = static_cast<std::int32_t>(static_cast<std::int16_t>(raw));
    out.push_back(m68k_branch_target(addr, signed_disp));
    out.push_back(next_pc);
    break;
  }
  case M68kInstructionKind::rts:
  case M68kInstructionKind::rte:
    break;
  case M68kInstructionKind::jmp:
    if (m68k_is_statically_foldable_control_ea(decoded.source_ea)) {
      out.push_back(m68k_canonical_ea_address(decoded.source_ea));
    } else if (const auto tier1 = accepted_tier1_by_source.find(addr);
               tier1 != accepted_tier1_by_source.end()) {
      // SEG-007-T190 / ADR-0031 (Stage B): this computed JMP has an
      // already-accepted post-stitch Tier-1 finite target set. Each proven
      // candidate is an ordinary successor of the enclosing subroutine body
      // for reachable-RTS purposes -- mutually exclusive with the foldable
      // branch above. `candidates` is already ordered/deduplicated.
      for (const auto &candidate : tier1->second.candidates) out.push_back(candidate.value);
    }
    break;
  case M68kInstructionKind::jsr:
  case M68kInstructionKind::bsr:
    out.push_back(next_pc);
    break;
  default:
    out.push_back(next_pc);
    break;
  }
  return out;
}
}  // namespace

std::vector<M68kStaticEdge> m68k_reachable_return_edges(
    const std::map<std::uint32_t, M68kDecodedInstruction> &decoded_by_address,
    const std::vector<M68kStaticFrame> &frames,
    const std::map<std::uint32_t, M68kIndirectTargetEaSet> &accepted_tier1_by_source) {
  std::set<Address> called_addresses;
  for (const auto &frame : frames) called_addresses.insert(frame.call.callee.value);
  std::map<Address, std::set<Address>> reachable_rts_of_callee;
  for (const auto callee : called_addresses) {
    std::set<Address> visited;
    std::vector<Address> worklist{callee};
    while (!worklist.empty()) {
      const auto pc = worklist.back();
      worklist.pop_back();
      if (!visited.insert(pc).second) continue;
      const auto found = decoded_by_address.find(pc);
      if (found == decoded_by_address.end()) continue;  // not canonically decoded: a safe, conservative dead end
      const auto &decoded = found->second;
      if (decoded.kind == M68kInstructionKind::rts) {
        reachable_rts_of_callee[callee].insert(pc);
        continue;
      }
      for (const auto successor : return_reachability_successors(pc, decoded, accepted_tier1_by_source))
        worklist.push_back(successor);
    }
  }
  // SEG-007-T151 correction: the emitted-edge identity must be the RTS
  // address together with the COMPLETE static-call identity (caller,
  // callee, continuation) -- mirroring the canonical `M68kStaticCall`
  // identity `same_call` already uses (platforms/genesis/machine/src/frontend.cpp)
  // and the same (caller, callee, continuation) triple the cross-seed
  // frame union already dedups frames by (`frame_signatures_seen`,
  // discover_m68k_general_startup). Keying only on (RTS, caller) is too
  // narrow: ADR-0009 and the existing multi-seed aggregation explicitly
  // allow one indirect call SOURCE instruction to legitimately own more
  // than one proven candidate frame sharing the same caller and
  // continuation but a DIFFERENT proven candidate callee. When two such
  // distinct candidate callees' own valid control paths both converge on
  // one shared RTS, each is a separate, independently valid
  // `return_to_continuation` edge for its own distinct frame and must be
  // preserved, not collapsed into one. Only an exact duplicate call frame
  // (identical caller, callee, and continuation) sharing the same RTS is a
  // true duplicate.
  std::vector<M68kStaticEdge> edges;
  std::set<std::tuple<Address, Address, Address, Address>> emitted;
  for (const auto &frame : frames) {
    const auto found = reachable_rts_of_callee.find(frame.call.callee.value);
    if (found == reachable_rts_of_callee.end()) continue;
    for (const auto rts_address : found->second) {  // std::set: sorted, deterministic
      const auto key = std::make_tuple(rts_address, frame.call.caller.source.address.value,
                                        frame.call.callee.value, frame.call.continuation.value);
      if (!emitted.insert(key).second) continue;
      edges.push_back(m68k_make_static_return_edge(frame.call, decoded_by_address.at(rts_address)));
    }
  }
  return edges;
}

M68kStaticCall m68k_make_static_call(const M68kDecodedInstruction &call,
                                     std::uint32_t callee_address) {
  const auto space = call.provenance.source.address.space;
  return {call.provenance,
          {space, static_cast<std::uint32_t>(call.provenance.source.address.value +
                                              call.provenance.length.value)},
          {space, callee_address}};
}

M68kStaticEdge m68k_make_static_call_edge(const M68kStaticCall &call) {
  return {call.caller, M68kStaticEdgeKind::direct_call, call.callee, call};
}

M68kStaticEdge m68k_make_static_return_edge(const M68kStaticCall &call,
                                             const M68kDecodedInstruction &return_instruction) {
  return {return_instruction.provenance, M68kStaticEdgeKind::return_to_continuation,
          call.continuation, call};
}

} // namespace segarecomp
