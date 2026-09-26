#include "segarecomp/cpu/m68k/static_discovery.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace segarecomp {
namespace {

M68kDiscoveryDecodeIssue decode_issue(const RejectedM68kDecode &rejection,
                                      const M68kInstructionSource &source) {
  M68kDiscoveryDecodeIssue issue{};
  issue.kind = rejection.outcome == DecodeOutcome::truncated_instruction
                   ? M68kDiscoveryDecodeIssueKind::truncated_instruction
               : rejection.outcome == DecodeOutcome::illegal_instruction
                   ? M68kDiscoveryDecodeIssueKind::illegal_instruction
               : rejection.unsupported_instruction_form
                   ? M68kDiscoveryDecodeIssueKind::unsupported_instruction_form
                   : M68kDiscoveryDecodeIssueKind::valid_but_unsupported_instruction;
  issue.address = source.source.address;
  if (rejection.has_provenance) {
    issue.provenance = rejection.provenance;
    issue.provenance->source.image_offset = source.provenance_image_offset;
  }
  if (issue.kind == M68kDiscoveryDecodeIssueKind::truncated_instruction) {
    issue.available_bytes = rejection.available_bytes;
    issue.requested_length = rejection.requested_length;
  }
  if (rejection.has_instruction_length) issue.instruction_length = rejection.instruction_length;
  return issue;
}

}  // namespace

const M68kDecodedInstruction *M68kStaticDecodeCache::find(M68kProgramAddress address) const {
  const auto found = entries_.find(address.value);
  return found == entries_.end() ? nullptr : &found->second;
}

M68kStaticDecodeResult M68kStaticDecodeCache::decode_or_get(
    const M68kInstructionSource &source, M68kDecodeProfile profile) {
  if (const auto *decoded = find(source.source.address)) return *decoded;
  const auto decoded = decode_m68k_instruction(source.bytes, source.source, profile);
  if (const auto *rejection = std::get_if<RejectedM68kDecode>(&decoded)) return decode_issue(*rejection, source);
  auto accepted = std::get<M68kDecodedInstruction>(decoded);
  accepted.provenance.source.image_offset = source.provenance_image_offset;
  const auto [it, inserted] = entries_.emplace(accepted.provenance.source.address.value, std::move(accepted));
  (void)inserted;
  return it->second;
}

namespace {

using Address = std::uint32_t;

// SEG-007-T124 / ADR-0009: the bounded forward register-value analysis that
// produces `M68kFiniteIndexValueSet`. `finite==false` is "unknown" (the ADR's
// default/fail-closed state); `finite==true` carries a sorted, deduplicated,
// non-empty (once used) set of possible low-word values, capped at 256
// members. This models exactly the two generic producers the ADR names
// (MOVEQ; ANDI.W #mask,Dn) and nothing else -- any other write to a Dn
// (including crossing a JSR/BSR call, whose callee body this bounded pass
// does not analyze) produces `unknown`, never a partial/inferred set.
struct M68kDnValueState {
  bool finite{false};
  std::vector<std::uint16_t> values;
};
// SEG-007-T178 / ADR-0009 producer extension: per-An state is a distinct
// typed fact -- `finite==false` is "unknown", `finite==true` carries a
// sorted, deduplicated, non-empty set of canonical 24-bit code addresses
// under the SAME hard 256 cap as the Dn set. Its only recognized producers
// are `LEA <foldable>,An` and `ADDA.W #imm,An`; every other write to that An
// forces `unknown`.
struct M68kAnValueState {
  bool finite{false};
  std::vector<std::uint32_t> addrs;
};

// SEG-007-T178 correction: the finite-An Tier-1 code-address proof is available
// for ordinary address registers A0-A6 only. A7/SP is intentionally excluded --
// it carries pervasive implicit stack mutations that `m68k_written_address_
// registers` cannot fully enumerate (PEA's `A7 -= 4`; LINK/UNLK's A7 side
// effect alongside the named frame register; the JSR/BSR callee-entry
// return-address push; RTS/RTE stack consumption), so a stale finite A7 proof
// (e.g. `LEA Target,A7 -> PEA (...) -> JMP (A7)`) would otherwise be possible.
// This exclusion is made explicit at the producer/consumer boundary rather
// than relying on every instruction writer forever enumerating implicit SP
// mutations. Runtime `(A7)` targets are left to a future one-shot / Tier-2
// `EmittedCodeAddressSet` mechanism (outside SEG-007-T178 scope).
inline constexpr std::uint8_t kM68kStackPointerRegister = 7U;
[[nodiscard]] inline constexpr bool m68k_an_finite_proof_eligible(std::uint8_t reg) noexcept {
  return reg < kM68kStackPointerRegister;
}
struct M68kRegisterState {
  std::array<M68kDnValueState, 8> dn{};
  std::array<M68kAnValueState, 8> an{};
};

bool m68k_dn_state_equal(const M68kDnValueState &left, const M68kDnValueState &right) {
  return left.finite == right.finite && left.values == right.values;
}

bool m68k_an_state_equal(const M68kAnValueState &left, const M68kAnValueState &right) {
  return left.finite == right.finite && left.addrs == right.addrs;
}

bool m68k_register_state_equal(const M68kRegisterState &left, const M68kRegisterState &right) {
  for (std::size_t index = 0; index < left.dn.size(); ++index)
    if (!m68k_dn_state_equal(left.dn[index], right.dn[index])) return false;
  for (std::size_t index = 0; index < left.an.size(); ++index)
    if (!m68k_an_state_equal(left.an[index], right.an[index])) return false;
  return true;
}

// ADR-0009: "equal register sets are retained and unequal finite sets are
// unioned only if the result remains within 256; otherwise that register
// becomes unknown." Merging with `unknown` on either side is always
// `unknown` (an unbounded/unproven predecessor state can never be unioned).
M68kDnValueState m68k_merge_dn_state(const M68kDnValueState &left, const M68kDnValueState &right) {
  if (m68k_dn_state_equal(left, right)) return left;
  if (!left.finite || !right.finite) return M68kDnValueState{};
  std::vector<std::uint16_t> merged = left.values;
  merged.insert(merged.end(), right.values.begin(), right.values.end());
  std::sort(merged.begin(), merged.end());
  merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
  if (merged.size() > 256U) return M68kDnValueState{};
  return M68kDnValueState{true, std::move(merged)};
}

// SEG-007-T178 / ADR-0009 producer extension: union An address sets under the
// same 256 cap; `unknown` on either side -> `unknown`; a union past the cap
// -> `unknown` (never a partial set).
M68kAnValueState m68k_merge_an_state(const M68kAnValueState &left, const M68kAnValueState &right) {
  if (m68k_an_state_equal(left, right)) return left;
  if (!left.finite || !right.finite) return M68kAnValueState{};
  std::vector<std::uint32_t> merged = left.addrs;
  merged.insert(merged.end(), right.addrs.begin(), right.addrs.end());
  std::sort(merged.begin(), merged.end());
  merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
  if (merged.size() > 256U) return M68kAnValueState{};
  return M68kAnValueState{true, std::move(merged)};
}

M68kRegisterState m68k_merge_register_state(const M68kRegisterState &left, const M68kRegisterState &right) {
  M68kRegisterState out{};
  for (std::size_t index = 0; index < out.dn.size(); ++index)
    out.dn[index] = m68k_merge_dn_state(left.dn[index], right.dn[index]);
  for (std::size_t index = 0; index < out.an.size(); ++index)
    out.an[index] = m68k_merge_an_state(left.an[index], right.an[index]);
  return out;
}

// Which Dn registers (0-7) a decoded instruction's ordinary architectural
// effect writes, independent of whether that write is one of the two
// recognized value producers below. This is a generic, kind-driven fact
// (never title-, table-, or input-specific): any decoded write to a Dn this
// bounded pass does not itself model as a value producer forces that
// register to `unknown` at the write site, per the ADR's "any other write to
// that Dn produces unknown" rule.
std::vector<std::uint8_t> m68k_written_data_registers(const M68kDecodedInstruction &decoded) {
  std::vector<std::uint8_t> registers;
  switch (decoded.kind) {
  case M68kInstructionKind::moveq:
    registers.push_back(static_cast<std::uint8_t>(decoded.destination));
    break;
  case M68kInstructionKind::subq_l_1_d0:
    registers.push_back(0U);
    break;
  case M68kInstructionKind::move:
  case M68kInstructionKind::clr:
  case M68kInstructionKind::not_operand:
  // SEG-021-T014: NEG/NEGX and ADDX/SUBX write a Dn destination (CMPM writes none).
  case M68kInstructionKind::negate_word:
  case M68kInstructionKind::negate_extended:
  case M68kInstructionKind::add_extended:
  case M68kInstructionKind::subtract_extended:
  // SEG-021-T015: ABCD/SBCD/NBCD write a Dn destination.
  case M68kInstructionKind::add_decimal:
  case M68kInstructionKind::subtract_decimal:
  case M68kInstructionKind::negate_decimal:
  // SEG-021-T016: Scc/TAS write a Dn destination; MOVEP writes Dn only in its memory-to-register direction.
  case M68kInstructionKind::set_conditional:
  case M68kInstructionKind::test_and_set:
  case M68kInstructionKind::movep:
  case M68kInstructionKind::add:
  case M68kInstructionKind::addi:
  case M68kInstructionKind::addq:
  case M68kInstructionKind::sub:
  case M68kInstructionKind::subi:
  case M68kInstructionKind::subq:
  case M68kInstructionKind::logical_and:
  case M68kInstructionKind::andi:
  case M68kInstructionKind::logical_or:
  case M68kInstructionKind::ori:
  case M68kInstructionKind::eor:
  case M68kInstructionKind::eori:
  case M68kInstructionKind::bchg:
  case M68kInstructionKind::bclr:
  case M68kInstructionKind::bset:
  case M68kInstructionKind::swap:
  case M68kInstructionKind::ext_w:
  case M68kInstructionKind::ext_l:
  case M68kInstructionKind::dbcc:
  case M68kInstructionKind::move_from_sr:
  case M68kInstructionKind::shift_rotate:
    if (decoded.destination_ea.mode == M68kEaMode::data_register) registers.push_back(decoded.destination_ea.reg);
    break;
  case M68kInstructionKind::exchange_registers:
    // SEG-021-T016: EXG writes both operands.
    for (const auto &operand : {decoded.source_ea, decoded.destination_ea})
      if (operand.mode == M68kEaMode::data_register) registers.push_back(operand.reg);
    break;
  case M68kInstructionKind::movem:
    if (decoded.movem_direction == M68kMovemDirection::memory_to_registers) {
      for (std::uint8_t bit = 0U; bit < 8U; ++bit)
        if ((decoded.movem_register_mask & static_cast<std::uint16_t>(1U << bit)) != 0U) registers.push_back(bit);
    }
    break;
  default:
    break;
  }
  return registers;
}

// SEG-007-T178 / ADR-0009 producer extension: which An registers (0-7) a
// decoded instruction's ordinary architectural effect writes or auto-updates,
// independent of whether that write is one of the two recognized An value
// producers. Any such write that is not itself modelled as a producer forces
// that An to `unknown` at the write site, per the ADR's "any other write...
// forces unknown" rule. Auto-update side effects on `(An)+`/`-(An)` in either
// operand position also clobber `ea.reg`.
std::vector<std::uint8_t> m68k_written_address_registers(const M68kDecodedInstruction &decoded) {
  std::vector<std::uint8_t> registers;
  const auto note_dest_an = [&] {
    if (decoded.destination_ea.mode == M68kEaMode::address_register)
      registers.push_back(decoded.destination_ea.reg);
  };
  switch (decoded.kind) {
  case M68kInstructionKind::movea:
  case M68kInstructionKind::adda:
  case M68kInstructionKind::suba:
  case M68kInstructionKind::lea:
  case M68kInstructionKind::addq:
  case M68kInstructionKind::subq:
  // SEG-007-T178 validator finding 1: LINK An,#d (An := SP) and UNLK An
  // (An := (SP)) both carry the written An in `destination_ea` (address_register
  // mode; decode.cpp m68k_decode_general_link_unlk) and neither is a recognized
  // An value producer, so both must force that An to `unknown` per the ADR rule.
  case M68kInstructionKind::link:
  case M68kInstructionKind::unlk:
  // SEG-021-T018: MOVE USP,An writes its fixed An destination.
  case M68kInstructionKind::move_usp_to_an:
    note_dest_an();
    break;
  // SEG-021-T018 / ADR 0043 §6: an SR write that changes S swaps the active stack pointer, so A7 is clobbered.
  case M68kInstructionKind::move_to_sr:
  case M68kInstructionKind::logical_immediate_to_sr:
  // SEG-021-T019: exception entry (TRAP/TRAPV/CHK/instruction-word exceptions) switches to and decrements the SSP;
  // RTR pops six bytes.
  case M68kInstructionKind::trap:
  case M68kInstructionKind::trapv:
  case M68kInstructionKind::chk:
  case M68kInstructionKind::rtr:
  case M68kInstructionKind::instruction_exception:
    registers.push_back(7U);
    break;
  case M68kInstructionKind::exchange_registers:
    // SEG-021-T016: EXG writes both operands (An in Ax,Ay and Dx,Ay).
    for (const auto &operand : {decoded.source_ea, decoded.destination_ea})
      if (operand.mode == M68kEaMode::address_register) registers.push_back(operand.reg);
    break;
  case M68kInstructionKind::movem:
    if (decoded.movem_direction == M68kMovemDirection::memory_to_registers) {
      for (std::uint8_t bit = 0U; bit < 8U; ++bit)
        if ((decoded.movem_register_mask & static_cast<std::uint16_t>(1U << (bit + 8U))) != 0U) registers.push_back(bit);
    }
    break;
  default:
    break;
  }
  for (const auto *ea : {&decoded.source_ea, &decoded.destination_ea})
    if (ea->mode == M68kEaMode::address_postinc || ea->mode == M68kEaMode::address_predec) registers.push_back(ea->reg);
  return registers;
}

// SEG-007-T161 / ADR-0022: the generation-time immutable in-cartridge
// offset-table descriptor finite-value producer. Given the decoded
// `MOVE.W (d8,PC,Xn),Dd` source EA and the proven finite selector state for
// the word data-register index Xn, returns the ordered, deduplicated set of
// 16-bit table-entry values read from the generation-time image, or
// `std::nullopt` on any failed proof obligation (the caller then folds the
// destination register to `unknown`, retaining ADR-0009's fail-closed
// frontier). See docs/decisions/0022-*.md for the full obligation list. Only
// the proven entry positions are inspected; no raw address/offset/byte/table
// identity is retained.
std::optional<std::vector<std::uint16_t>> m68k_fold_immutable_offset_table(
    const M68kEffectiveAddress &source_ea, const M68kDnValueState &selector,
    M68kStaticDiscoveryEnvironment &environment) {
  if (source_ea.mode != M68kEaMode::pc_index8) return std::nullopt;
  if (source_ea.index_is_address || source_ea.index_is_long) return std::nullopt;  // word Dn index only
  if (!selector.finite || selector.values.empty() || selector.values.size() > 256U) return std::nullopt;

  std::vector<std::int64_t> entry_addresses;
  entry_addresses.reserve(selector.values.size());
  for (const auto low_word : selector.values) {
    const auto index_value = static_cast<std::int32_t>(static_cast<std::int16_t>(low_word));
    const auto raw = static_cast<std::int64_t>(source_ea.pc_base_address) +
                      static_cast<std::int64_t>(source_ea.displacement) + static_cast<std::int64_t>(index_value);
    if (raw < 0 || raw > static_cast<std::int64_t>(UINT32_C(0x00FFFFFF))) return std::nullopt;  // non-24-bit / overflow
    if ((raw & 1) != 0) return std::nullopt;  // MOVE.W from an odd address: architecturally invalid
    entry_addresses.push_back(raw);
  }
  const auto lo = *std::min_element(entry_addresses.begin(), entry_addresses.end());
  const auto hi = *std::max_element(entry_addresses.begin(), entry_addresses.end());
  const auto span_bytes = static_cast<std::uint64_t>(hi - lo) + 2U;
  if (span_bytes > UINT64_C(0x10000)) return std::nullopt;  // bounded descriptor only

  const auto block = environment.read_immutable_cartridge_bytes(
      {TargetAddressSpace::m68k_program, static_cast<std::uint32_t>(lo)}, static_cast<std::uint32_t>(span_bytes));
  if (!block || block->bytes.size() != span_bytes) return std::nullopt;  // unmapped / partially mapped / not immutable

  std::vector<std::uint16_t> values;
  values.reserve(entry_addresses.size());
  for (const auto addr : entry_addresses) {
    const auto offset = static_cast<std::size_t>(addr - lo);
    const auto entry = static_cast<std::uint16_t>((static_cast<std::uint16_t>(block->bytes[offset]) << 8) |
                                                   static_cast<std::uint16_t>(block->bytes[offset + 1U]));
    values.push_back(entry);
  }
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  if (values.size() > 256U) return std::nullopt;  // existing 256-member finite-value cap
  return values;
}

// The one instruction-level transfer function: MOVEQ and word-size ANDI #
// mask,Dn are the two ADR-0009 producers; the ADR-0022 immutable offset-table
// read is the additive third producer; every other Dn write forces
// `unknown`, and every non-writing instruction leaves the incoming state
// unchanged.
M68kRegisterState m68k_apply_finite_value_transfer(const M68kRegisterState &in, const M68kDecodedInstruction &decoded,
                                                    M68kStaticDiscoveryEnvironment *environment) {
  auto out = in;
  // SEG-007-T161 / ADR-0022: additive producer -- a word MOVE from a
  // brief-format PC-relative indexed immutable cartridge offset table whose
  // selector is already finitely proven folds to the finite set of entry
  // values; any failed proof obligation falls through to `unknown` exactly
  // like "any other write".
  if (environment != nullptr && decoded.kind == M68kInstructionKind::move &&
      decoded.size == M68kMemoryAccessWidth::word &&
      decoded.destination_ea.mode == M68kEaMode::data_register &&
      decoded.source_ea.mode == M68kEaMode::pc_index8) {
    const auto reg = decoded.destination_ea.reg;
    // SEG-007-T164 / ADR-0023: an alternate producer of the same finite
    // selector-value input `m68k_fold_immutable_offset_table` already
    // consumes -- not a new parameter threaded into that function. When an
    // opted-in, ROM-hash-verified annotation exactly matches this site's own
    // recognized table base and entry width, its synthetic ordered position
    // set `{0, stride_bytes, ..., (entry_count-1)*stride_bytes}` replaces the
    // register-proven `selector` below; otherwise (no annotation, or a
    // non-matching base/width) the existing register-proven selector is used
    // completely unchanged, exactly as before this task.
    M68kDnValueState selector = in.dn[decoded.source_ea.index_reg];
    if (!decoded.source_ea.index_is_address && !decoded.source_ea.index_is_long) {
      const auto base = static_cast<Address>(static_cast<std::int64_t>(decoded.source_ea.pc_base_address) +
                                              decoded.source_ea.displacement);
      if (const auto hint = environment->logical_table_descriptor_hint(
              {TargetAddressSpace::m68k_program, base}, 2U)) {
        // Reject a cap-exceeding `entry_count` explicitly, upfront, rather
        // than relying on `m68k_fold_immutable_offset_table`'s own
        // `selector.values.size() > 256U` check to observe it after
        // construction: for a `stride_bytes` that is a multiple of 256 (or
        // shares any other factor with 65536 that collapses the built
        // progression's cardinality), the raw arithmetic-progression
        // positions `{0, stride_bytes, ..., (entry_count-1)*stride_bytes}
        // mod 65536` can have a period well under `entry_count`, so
        // deduplication alone would silently produce a set of at most 256
        // unique members for an arbitrarily large (even adversarially
        // wrong/typo'd) `entry_count`, defeating the deterministic cap-trip
        // this mechanism must guarantee. Bounding `entry_count` itself here
        // is the only construction that is correct for every `stride_bytes`.
        // Second hardening (still SEG-007-T164 / ADR-0023): the largest
        // implied position, `(entry_count-1)*stride_bytes`, must itself be
        // representable within the byte-offset domain this mechanism is
        // permitted to construct, computed with checked (`std::uint64_t`)
        // arithmetic so the multiplication itself cannot silently overflow.
        // `m68k_fold_immutable_offset_table` above independently enforces
        // `max_entry_addr - min_entry_addr + 2 <= 0x10000` (ADR-0022 Decision
        // §4) over the ACTUAL entry addresses it computes from whatever
        // selector it is given; applied to a synthetic position set that
        // always starts at 0, the tightest bound this producer can pre-check
        // without duplicating that address arithmetic is the same span
        // formula with `min = 0`: `max_offset + 2 <= 0x10000`, i.e.
        // `max_offset <= 0xFFFE`. That is at least as strict as the simpler
        // "fits in 16 bits" (`<= 0xFFFF`) bound alone. Without this check, a
        // large `stride_bytes` combined with an `entry_count` up to 256 can
        // make `(entry_count-1)*stride_bytes` exceed 65535; the old
        // `& 0xFFFFU` truncation would then silently wrap that value into a
        // small, structurally-plausible-looking in-range position that does
        // NOT correspond to the actual logically-intended (unwrapped) table
        // position -- exactly the collision class this check closes. Any
        // failure here is a precondition failure like any other: no
        // synthetic selector is constructed, and the existing register-
        // proven `selector` is used completely unchanged.
        constexpr std::uint64_t kMaxImpliedOffset = UINT64_C(0xFFFE);
        if (hint->entry_count > 0U && hint->entry_count <= 256U && hint->stride_bytes > 0U) {
          const auto max_implied_offset =
              static_cast<std::uint64_t>(hint->entry_count - 1U) * static_cast<std::uint64_t>(hint->stride_bytes);
          if (max_implied_offset <= kMaxImpliedOffset) {
            std::vector<std::uint16_t> positions;
            positions.reserve(static_cast<std::size_t>(hint->entry_count));
            for (std::uint64_t index = 0; index < hint->entry_count; ++index) {
              const auto position = index * static_cast<std::uint64_t>(hint->stride_bytes);
              // Defensive only: `position` is already proven `<= kMaxImpliedOffset
              // <= 0xFFFE` by the upfront check above, so this mask can never
              // actually truncate a construction this branch admits.
              positions.push_back(static_cast<std::uint16_t>(position & 0xFFFFU));
            }
            std::sort(positions.begin(), positions.end());
            positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
            selector = M68kDnValueState{true, std::move(positions)};
          }
        }
      }
    }
    auto folded = m68k_fold_immutable_offset_table(decoded.source_ea, selector, *environment);
    out.dn[reg] = folded ? M68kDnValueState{true, std::move(*folded)} : M68kDnValueState{};
    return out;
  }
  // SEG-007-T178 / ADR-0009 producer extension: the two recognized An
  // code-address producers. Handled before the generic An invalidation loop
  // below so a recognized producer's proven set is not immediately clobbered.
  if (decoded.kind == M68kInstructionKind::lea &&
      decoded.destination_ea.mode == M68kEaMode::address_register &&
      m68k_an_finite_proof_eligible(decoded.destination_ea.reg) &&
      m68k_is_statically_foldable_control_ea(decoded.source_ea)) {
    out.an[decoded.destination_ea.reg] =
        M68kAnValueState{true, {m68k_canonical_ea_address(decoded.source_ea) & UINT32_C(0x00FFFFFF)}};
    return out;
  }
  if (decoded.kind == M68kInstructionKind::adda &&
      decoded.size == M68kMemoryAccessWidth::word &&
      decoded.source_ea.mode == M68kEaMode::immediate &&
      decoded.destination_ea.mode == M68kEaMode::address_register &&
      m68k_an_finite_proof_eligible(decoded.destination_ea.reg)) {
    const auto reg = decoded.destination_ea.reg;
    const auto delta = static_cast<std::int32_t>(
        static_cast<std::int16_t>(static_cast<std::uint16_t>(decoded.source_ea.immediate_value & 0xFFFFU)));
    const auto &current = in.an[reg];
    if (current.finite) {
      std::vector<std::uint32_t> mapped;
      mapped.reserve(current.addrs.size());
      for (const auto member : current.addrs)
        mapped.push_back((static_cast<std::uint32_t>(member) + static_cast<std::uint32_t>(delta)) & UINT32_C(0x00FFFFFF));
      std::sort(mapped.begin(), mapped.end());
      mapped.erase(std::unique(mapped.begin(), mapped.end()), mapped.end());
      out.an[reg] = M68kAnValueState{true, std::move(mapped)};
    } else {
      out.an[reg] = M68kAnValueState{};
    }
    return out;
  }
  if (decoded.kind == M68kInstructionKind::moveq) {
    const auto reg = static_cast<std::uint8_t>(decoded.destination);
    const auto low_word = static_cast<std::uint16_t>(static_cast<std::int32_t>(decoded.operand));
    out.dn[reg] = M68kDnValueState{true, {low_word}};
    return out;
  }
  if (decoded.kind == M68kInstructionKind::andi && decoded.size == M68kMemoryAccessWidth::word &&
      decoded.destination_ea.mode == M68kEaMode::data_register) {
    const auto reg = decoded.destination_ea.reg;
    const auto mask = static_cast<std::uint16_t>(decoded.source_ea.immediate_value & 0xFFFFU);
    const auto &current = in.dn[reg];
    if (current.finite) {
      std::vector<std::uint16_t> masked;
      masked.reserve(current.values.size());
      for (const auto value : current.values) masked.push_back(static_cast<std::uint16_t>(value & mask));
      std::sort(masked.begin(), masked.end());
      masked.erase(std::unique(masked.begin(), masked.end()), masked.end());
      out.dn[reg] = M68kDnValueState{true, std::move(masked)};
    } else if (static_cast<unsigned>(std::popcount(mask)) <= 8U) {
      // ADR-0009: an unknown input maps to the finite set of all low-word
      // submasks of `mask` when popcount(mask) <= 8 (at most 256 submasks).
      std::vector<std::uint16_t> submasks;
      std::uint32_t submask = mask;
      for (;;) {
        submasks.push_back(static_cast<std::uint16_t>(submask));
        if (submask == 0U) break;
        submask = (submask - 1U) & mask;
      }
      std::sort(submasks.begin(), submasks.end());
      out.dn[reg] = M68kDnValueState{true, std::move(submasks)};
    } else {
      out.dn[reg] = M68kDnValueState{};
    }
    return out;
  }
  // SEG-007-T193 / ADR-0009 producer extension (A): a bounded word immediate
  // logical-right-shift, `LSR.W #imm,Dn` -- the register-form shift/rotate
  // shape with `shift_rotate_kind == lsr`, `size == word`, an immediate count
  // source (never a register-count shift), and a Dn destination (never the
  // memory form). For a finite incoming low-word set, applies exact 16-bit
  // logical-right-shift semantics to every member; any other shift/rotate
  // family, size, or count-source form is left to the generic wipe below,
  // exactly like every other unmodelled write.
  if (decoded.kind == M68kInstructionKind::shift_rotate && decoded.shift_rotate_kind == M68kShiftRotateKind::lsr &&
      decoded.size == M68kMemoryAccessWidth::word && decoded.destination_ea.mode == M68kEaMode::data_register &&
      decoded.source_ea.mode == M68kEaMode::immediate) {
    const auto reg = decoded.destination_ea.reg;
    const auto &current = in.dn[reg];
    if (current.finite) {
      const auto count = static_cast<unsigned>(decoded.source_ea.immediate_value & 0xFFU);
      std::vector<std::uint16_t> shifted;
      shifted.reserve(current.values.size());
      for (const auto value : current.values)
        shifted.push_back(count >= 16U ? std::uint16_t{0} : static_cast<std::uint16_t>(value >> count));
      std::sort(shifted.begin(), shifted.end());
      shifted.erase(std::unique(shifted.begin(), shifted.end()), shifted.end());
      out.dn[reg] = shifted.size() > 256U ? M68kDnValueState{} : M68kDnValueState{true, std::move(shifted)};
    } else {
      out.dn[reg] = M68kDnValueState{};
    }
    return out;
  }
  // SEG-007-T193 / ADR-0009 producer extension (B): a bounded word self-add,
  // the exact generic form `ADD.W Dn,Dn` (source and destination the same
  // data register). For a finite incoming low-word set, applies exact 16-bit
  // wrapping add-to-self semantics to every member. Never generalizes to
  // `ADD Dx,Dy` with differing registers, and never infers a Cartesian
  // product -- any other operand shape falls through to the generic wipe.
  if (decoded.kind == M68kInstructionKind::add && decoded.size == M68kMemoryAccessWidth::word &&
      decoded.source_ea.mode == M68kEaMode::data_register && decoded.destination_ea.mode == M68kEaMode::data_register &&
      decoded.source_ea.reg == decoded.destination_ea.reg) {
    const auto reg = decoded.destination_ea.reg;
    const auto &current = in.dn[reg];
    if (current.finite) {
      std::vector<std::uint16_t> doubled;
      doubled.reserve(current.values.size());
      for (const auto value : current.values)
        doubled.push_back(static_cast<std::uint16_t>(static_cast<std::uint32_t>(value) + static_cast<std::uint32_t>(value)));
      std::sort(doubled.begin(), doubled.end());
      doubled.erase(std::unique(doubled.begin(), doubled.end()), doubled.end());
      out.dn[reg] = doubled.size() > 256U ? M68kDnValueState{} : M68kDnValueState{true, std::move(doubled)};
    } else {
      out.dn[reg] = M68kDnValueState{};
    }
    return out;
  }
  // SEG-007-T199 / ADR-0009 owner-2 (same proven chain as owner-1): a byte
  // MOVE from memory into a data register writes bits 0-7 only; bits 8-31 keep
  // their prior value. When the prior low-word state is finite and every
  // member shares the same bits 8-15, the post-load low word is the finite set
  // `{ high | k : k in 0..255 }` (exactly 256 members, within the cap). An
  // immediate or register source, or a prior member disagreeing on bits 8-15,
  // falls through to the generic wipe.
  if (decoded.kind == M68kInstructionKind::move && decoded.size == M68kMemoryAccessWidth::byte &&
      decoded.destination_ea.mode == M68kEaMode::data_register &&
      decoded.source_ea.mode != M68kEaMode::data_register &&
      decoded.source_ea.mode != M68kEaMode::immediate &&
      decoded.source_ea.mode != M68kEaMode::address_register &&
      decoded.source_ea.mode != M68kEaMode::unused) {
    const auto reg = decoded.destination_ea.reg;
    const auto &current = in.dn[reg];
    if (current.finite && !current.values.empty()) {
      const std::uint16_t high = current.values.front() & 0xFF00U;
      const bool uniform_high =
          std::all_of(current.values.begin(), current.values.end(),
                      [&](std::uint16_t v) { return (v & 0xFF00U) == high; });
      if (uniform_high) {
        std::vector<std::uint16_t> loaded;
        loaded.reserve(256U);
        for (std::uint32_t k = 0; k < 256U; ++k) loaded.push_back(static_cast<std::uint16_t>(high | k));
        out.dn[reg] = M68kDnValueState{true, std::move(loaded)};
        return out;
      }
    }
    out.dn[reg] = M68kDnValueState{};
    return out;
  }
  // SEG-007-T199 / ADR-0009 owner-2: a bounded immediate subtraction
  // normalizing a finite selector -- `SUBI`/`SUBQ #imm,Dn`, byte or word size,
  // data-register destination. Byte subtracts from bits 0-7 only (bits 8-15
  // preserved); word subtracts from bits 0-15. Any other operand shape or an
  // unknown input falls through to the generic wipe.
  if ((decoded.kind == M68kInstructionKind::subi || decoded.kind == M68kInstructionKind::subq) &&
      decoded.destination_ea.mode == M68kEaMode::data_register &&
      (decoded.size == M68kMemoryAccessWidth::byte || decoded.size == M68kMemoryAccessWidth::word)) {
    const auto reg = decoded.destination_ea.reg;
    const auto &current = in.dn[reg];
    if (current.finite) {
      const auto imm = decoded.source_ea.immediate_value;
      const bool byte_size = decoded.size == M68kMemoryAccessWidth::byte;
      std::vector<std::uint16_t> reduced;
      reduced.reserve(current.values.size());
      for (const auto v : current.values) {
        if (byte_size)
          reduced.push_back(static_cast<std::uint16_t>((v & 0xFF00U) |
                                                       ((static_cast<std::uint32_t>(v) - imm) & 0xFFU)));
        else
          reduced.push_back(static_cast<std::uint16_t>((static_cast<std::uint32_t>(v) - imm) & 0xFFFFU));
      }
      std::sort(reduced.begin(), reduced.end());
      reduced.erase(std::unique(reduced.begin(), reduced.end()), reduced.end());
      out.dn[reg] = M68kDnValueState{true, std::move(reduced)};
    } else {
      out.dn[reg] = M68kDnValueState{};
    }
    return out;
  }
  // SEG-007-T199 / ADR-0009 owner-2: a bounded word immediate LEFT shift,
  // `LSL.W #imm,Dn` -- the exact sibling of the existing T193 `LSR.W #imm,Dn`
  // rule (register form, immediate count, Dn destination). Applies exact
  // 16-bit left-shift semantics to every finite member; a count >= 16 yields
  // zero. Any other shift/rotate family, size, or count-source form is left to
  // the generic wipe.
  if (decoded.kind == M68kInstructionKind::shift_rotate &&
      decoded.shift_rotate_kind == M68kShiftRotateKind::lsl &&
      decoded.size == M68kMemoryAccessWidth::word &&
      decoded.destination_ea.mode == M68kEaMode::data_register &&
      decoded.source_ea.mode == M68kEaMode::immediate) {
    const auto reg = decoded.destination_ea.reg;
    const auto &current = in.dn[reg];
    if (current.finite) {
      // The decoder already resolves the 3-bit quick shift-count field's
      // architectural 0->8 mapping (`m68k_decode_general_shift_rotate`), so a
      // `LSL.W #0` encoding arrives here as `immediate_value == 8`. No 0-means-8
      // fixup is needed (or wanted) at this layer.
      const auto count = static_cast<unsigned>(decoded.source_ea.immediate_value & 0xFFU);
      std::vector<std::uint16_t> shifted;
      shifted.reserve(current.values.size());
      for (const auto v : current.values)
        shifted.push_back(count >= 16U ? std::uint16_t{0}
                                       : static_cast<std::uint16_t>((static_cast<std::uint32_t>(v) << count) & 0xFFFFU));
      std::sort(shifted.begin(), shifted.end());
      shifted.erase(std::unique(shifted.begin(), shifted.end()), shifted.end());
      out.dn[reg] = shifted.size() > 256U ? M68kDnValueState{} : M68kDnValueState{true, std::move(shifted)};
    } else {
      out.dn[reg] = M68kDnValueState{};
    }
    return out;
  }
  for (const auto reg : m68k_written_data_registers(decoded)) out.dn[reg] = M68kDnValueState{};
  for (const auto reg : m68k_written_address_registers(decoded)) out.an[reg] = M68kAnValueState{};
  return out;
}

// One propagated successor of a decoded instruction, for value-flow purposes
// only (never a control-transfer decision -- that remains M68kStaticGraphWalker
// ::walk's sole responsibility). `force_unknown` models a JSR/BSR call's own
// static continuation: this bounded pass does not analyze callee bodies, so
// any register state observed after a call returns is conservatively
// `unknown`, per the ADR's "any other write... produces unknown" rule
// applied to an unmodeled callee effect.
struct M68kValueFlowSuccessor {
  Address address{};
  bool force_unknown{};
  // SEG-007-T178 / ADR-0009 producer extension: set only on the JSR/BSR
  // post-call continuation successor, and only when the callee entry address
  // is itself statically known (BSR displacement, or a foldable JSR control
  // EA). When present, the continuation merge may preserve An/Dn finite sets
  // the callee provably never writes instead of wiping the whole state; when
  // absent (unknown callee), the existing whole-state-unknown behavior stands.
  std::optional<Address> callee_entry{};
  // SEG-007-T199 / ADR-0009 owner-1: an edge-local finite-domain constraint on
  // one Dn low-word, derived from a `CMPI/CMP #imm,Dn` immediately followed by
  // an unsigned conditional branch (`Bcc` with condition hi/ls/cc/cs, or the
  // equality side of eq/ne). Purely a value-flow refinement applied at
  // contribution time to THIS successor edge; never a control-transfer
  // decision. `width_mask` is 0xFF for a byte compare (constrains bits 0-7
  // only) or 0xFFFF for a word compare. On a finite incoming set, members
  // whose masked low part falls outside `[lo, hi]` are dropped on this edge.
  // On an `unknown` incoming state the finite set `{lo..hi}` is materialized
  // only for a word compare (`width_mask == 0xFFFF`) with `hi - lo + 1 <= 256`
  // -- a byte compare cannot bound bits 8-15 from `unknown`, so it stays
  // `unknown` (fail-closed). An unsatisfiable range yields the empty finite
  // set (that edge is unreachable for this path).
  struct DnConstraint {
    std::uint8_t reg{};
    std::uint32_t lo{};
    std::uint32_t hi{};
    std::uint32_t width_mask{};
    bool empty{};
  };
  std::optional<DnConstraint> dn_constraint{};
};

// SEG-007-T199 / ADR-0009 owner-1: apply one edge-local Dn constraint to a
// contributed value-flow state. Monotone toward `unknown`/smaller finite sets
// in the ADR-0009 lattice (it only ever removes members or materializes a
// bounded finite set from `unknown`), so the surrounding worklist fixed point
// still terminates.
inline void m68k_apply_dn_edge_constraint(M68kRegisterState &state,
                                          const M68kValueFlowSuccessor::DnConstraint &c) {
  auto &slot = state.dn[c.reg];
  if (c.empty) {
    slot = M68kDnValueState{true, {}};
    return;
  }
  if (slot.finite) {
    std::vector<std::uint16_t> kept;
    kept.reserve(slot.values.size());
    for (const auto v : slot.values) {
      const std::uint32_t masked = static_cast<std::uint32_t>(v) & c.width_mask;
      if (masked >= c.lo && masked <= c.hi) kept.push_back(v);
    }
    slot = M68kDnValueState{true, std::move(kept)};
    return;
  }
  // `unknown` incoming: materialize only when the full low word is pinned
  // (word compare) and the interval is within the existing 256-member cap.
  if (c.width_mask == 0xFFFFU && c.hi >= c.lo && (c.hi - c.lo + 1U) <= 256U) {
    std::vector<std::uint16_t> materialized;
    materialized.reserve(static_cast<std::size_t>(c.hi - c.lo + 1U));
    for (std::uint32_t value = c.lo; value <= c.hi; ++value)
      materialized.push_back(static_cast<std::uint16_t>(value));
    slot = M68kDnValueState{true, std::move(materialized)};
  }
}

// SEG-007-T199 / ADR-0009 owner-1: given the decoded predecessor `CMPI/CMP
// #imm,Dn` and the decoded unsigned conditional branch that immediately falls
// through from it, fill the taken-edge and not-taken-edge Dn constraints.
// Returns false (no constraint) for any non-immediate compare, any long-size
// compare, any signed/overflow/sign condition, or a non-data-register compared
// operand. `reg` is the compared Dn; `taken`/`nt` are set only when that side
// carries a usable bound.
inline bool m68k_derive_unsigned_compare_branch_constraints(
    const M68kDecodedInstruction &compare, const M68kDecodedInstruction &branch, std::uint8_t &reg,
    std::optional<M68kValueFlowSuccessor::DnConstraint> &taken,
    std::optional<M68kValueFlowSuccessor::DnConstraint> &nt) {
  const bool is_cmpi = compare.kind == M68kInstructionKind::cmpi &&
                       compare.destination_ea.mode == M68kEaMode::data_register;
  const bool is_cmp_imm = compare.kind == M68kInstructionKind::cmp &&
                          compare.source_ea.mode == M68kEaMode::immediate &&
                          compare.destination_ea.mode == M68kEaMode::data_register;
  if (!is_cmpi && !is_cmp_imm) return false;
  std::uint32_t width_mask = 0U;
  if (compare.size == M68kMemoryAccessWidth::byte) width_mask = 0xFFU;
  else if (compare.size == M68kMemoryAccessWidth::word) width_mask = 0xFFFFU;
  else return false;  // long compare: index is word-sized, no clean mapping
  M68kCondition cond{};
  if (branch.kind == M68kInstructionKind::bne_short) cond = M68kCondition::ne;
  else if (branch.kind == M68kInstructionKind::branch) cond = branch.condition;
  else return false;

  reg = compare.destination_ea.reg;
  const std::uint32_t m = compare.source_ea.immediate_value & width_mask;
  const std::uint32_t w = width_mask;
  using DC = M68kValueFlowSuccessor::DnConstraint;
  const auto lower = [&](std::uint32_t lo, std::uint32_t hi) -> std::optional<DC> {
    if (lo > hi) return DC{reg, 1U, 0U, w, true};
    return DC{reg, lo, hi, w, false};
  };
  switch (cond) {
  case M68kCondition::cc:  // C clear <=> Dn >=u m
    taken = lower(m, w);
    nt = (m == 0U) ? lower(1U, 0U) : lower(0U, m - 1U);
    return true;
  case M68kCondition::cs:  // C set <=> Dn <u m
    taken = (m == 0U) ? lower(1U, 0U) : lower(0U, m - 1U);
    nt = lower(m, w);
    return true;
  case M68kCondition::hi:  // !C & !Z <=> Dn >u m
    taken = (m >= w) ? lower(1U, 0U) : lower(m + 1U, w);
    nt = lower(0U, m);
    return true;
  case M68kCondition::ls:  // C | Z <=> Dn <=u m
    taken = lower(0U, m);
    nt = (m >= w) ? lower(1U, 0U) : lower(m + 1U, w);
    return true;
  case M68kCondition::eq:  // Z <=> Dn ==u m
    taken = lower(m, m);
    return true;
  case M68kCondition::ne:  // !Z <=> Dn !=u m
    nt = lower(m, m);
    return true;
  default:
    reg = 0U;
    return false;  // signed / sign / overflow condition: fail-closed
  }
}

// SEG-007-T188 / ADR-0029: decode-local `next_pc`-class successors only. The
// canonical validated stitched control-edge relation
// (`std::vector<M68kStaticEdge>`) is the sole authority for every
// non-fallthrough control transfer (taken branches, jmp/jsr targets, resolved
// finite indirect candidates, stitched continuations). That relation carries
// no plain sequential fallthrough edge, so this helper still supplies every
// `next_pc` successor: sequential fallthrough, the not-taken side of a
// conditional branch, and a call site's own post-call continuation
// (`force_unknown`; the caller fills `callee_entry` from the matching
// canonical direct-call edge). It emits NO taken-branch / jmp / callee-entry /
// indirect-target successor.
std::vector<M68kValueFlowSuccessor> m68k_decode_local_flow_successors(Address addr,
                                                                     const M68kDecodedInstruction &decoded) {
  std::vector<M68kValueFlowSuccessor> out;
  const auto next_pc = static_cast<Address>(addr + decoded.provenance.length.value);
  switch (decoded.kind) {
  case M68kInstructionKind::bne_short:
    out.push_back({next_pc, false});
    break;
  case M68kInstructionKind::bra_short:
    break;
  case M68kInstructionKind::branch:
    if (decoded.condition != M68kCondition::always) out.push_back({next_pc, false});
    break;
  case M68kInstructionKind::dbcc:
    out.push_back({next_pc, false});
    break;
  case M68kInstructionKind::rts:
  case M68kInstructionKind::rte:
  case M68kInstructionKind::rtr:                    // SEG-021-T019: PC popped from the stack frame
  case M68kInstructionKind::instruction_exception:  // SEG-021-T019: always the vector; no successor
  case M68kInstructionKind::jmp:
    break;
  case M68kInstructionKind::jsr:
  case M68kInstructionKind::bsr:
  // SEG-021-T019: TRAP (always) and TRAPV/CHK (conditionally) enter a handler whose RTE resumes at the next
  // instruction; the handler may change any register, so that continuation carries no register fact (like a call's).
  case M68kInstructionKind::trap:
  case M68kInstructionKind::trapv:
  case M68kInstructionKind::chk:
    out.push_back({next_pc, true, std::nullopt});
    break;
  default:
    out.push_back({next_pc, false});
    break;
  }
  return out;
}

// SEG-007-T188 / ADR-0029: per-source-address value-flow successor lists
// derived from the canonical validated stitched control-edge relation, unioned
// with the decode-local `next_pc` successors above. This is the single graph
// both post-stitch consumers (`m68k_select_stitched_analysis_roots` and the
// ADR-0009 finite-register fixed point) bind to.
//
//   direct_branch / indirect_branch      -> plain successor(target)
//   direct_call / indirect_call          -> plain successor(callee)
//                                           + the decode-local force_unknown
//                                             continuation, whose callee_entry
//                                             is filled from a direct_call edge
//   fallthrough / fallthrough_continuation-> plain successor(target)
//   return_to_continuation               -> omitted (structural return topology
//                                           is owned by synthesize_return_edges
//                                           / the runtime membership check;
//                                           including it here would churn the
//                                           T186 root-selection topology)
//
// Result lists are deterministically sorted and de-duplicated on the
// (address, force_unknown, callee_entry) triple. `entries` bounds membership.
// SEG-007-T189 / ADR-0030: single callee per direct-call source instruction
// address, extracted so both the canonical adjacency builder (which uses it
// to fill the decode-local continuation's `callee_entry` for the bounded
// callee register-write footprint proof) and the bounded call-context-
// sensitive An finite-register proof (which uses it to identify each
// distinguishable call-site context) share one definition. An indirect_call
// or a direct_call with no resolved `edge.call` never contributes an entry --
// only a statically known single callee is a valid context-identity anchor.
std::map<Address, Address> m68k_direct_call_sites(const std::vector<M68kStaticEdge> &canonical_edges) {
  std::map<Address, Address> direct_call_callee;
  for (const auto &edge : canonical_edges) {
    if (edge.kind == M68kStaticEdgeKind::direct_call && edge.call)
      direct_call_callee[edge.source_instruction.source.address.value] = edge.call->callee.value;
  }
  return direct_call_callee;
}

std::map<Address, std::vector<M68kValueFlowSuccessor>> m68k_canonical_control_adjacency(
    const std::map<Address, M68kDecodedInstruction> &entries,
    const std::vector<M68kStaticEdge> &canonical_edges) {
  // Single callee per call site (direct_call only) -- used to fill the
  // decode-local continuation's `callee_entry` for the bounded callee
  // register-write footprint proof.
  const auto direct_call_callee = m68k_direct_call_sites(canonical_edges);

  std::map<Address, std::vector<M68kValueFlowSuccessor>> adjacency;
  for (const auto &[address, decoded] : entries) {
    auto &out = adjacency[address];
    for (auto succ : m68k_decode_local_flow_successors(address, decoded)) {
      if (succ.force_unknown && !succ.callee_entry) {
        if (const auto it = direct_call_callee.find(address); it != direct_call_callee.end())
          succ.callee_entry = it->second;
      }
      out.push_back(succ);
    }
  }

  for (const auto &edge : canonical_edges) {
    const auto source = edge.source_instruction.source.address.value;
    if (!entries.contains(source)) continue;
    auto &out = adjacency[source];
    switch (edge.kind) {
    case M68kStaticEdgeKind::direct_branch:
    case M68kStaticEdgeKind::indirect_branch:
    case M68kStaticEdgeKind::fallthrough:
    case M68kStaticEdgeKind::fallthrough_continuation:
      out.push_back({edge.target.value, false, std::nullopt});
      break;
    case M68kStaticEdgeKind::direct_call:
    case M68kStaticEdgeKind::indirect_call:
      if (edge.call) out.push_back({edge.call->callee.value, false, std::nullopt});
      else out.push_back({edge.target.value, false, std::nullopt});
      break;
    case M68kStaticEdgeKind::return_to_continuation:
      break;
    }
  }

  for (auto &[address, out] : adjacency) {
    (void)address;
    std::sort(out.begin(), out.end(), [](const M68kValueFlowSuccessor &a, const M68kValueFlowSuccessor &b) {
      if (a.address != b.address) return a.address < b.address;
      if (a.force_unknown != b.force_unknown) return a.force_unknown < b.force_unknown;
      return a.callee_entry.value_or(0U) < b.callee_entry.value_or(0U);
    });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const M68kValueFlowSuccessor &a, const M68kValueFlowSuccessor &b) {
                            return a.address == b.address && a.force_unknown == b.force_unknown &&
                                   a.callee_entry == b.callee_entry;
                          }),
              out.end());
  }

  // SEG-007-T199 / ADR-0009 owner-1: attach edge-local Dn finite-domain
  // constraints for the `CMPI/CMP #imm,Dn` + immediately-following unsigned
  // `Bcc` shape. The compare must be the exact contiguous fallthrough
  // predecessor of the branch (nothing between them that could touch flags or
  // the register). The taken edge is the branch's non-`next_pc`,
  // non-`force_unknown` successor; the not-taken edge is `next_pc`.
  std::map<Address, Address> fallthrough_pred;  // (instr end) -> (instr start)
  for (const auto &[address, decoded] : entries)
    fallthrough_pred[static_cast<Address>(address + decoded.provenance.length.value)] = address;
  // SEG-007-T199 F1: the edge constraint is sound only when the compare's flags
  // provably executed on every path into the branch -- i.e. the branch's ONLY
  // in-edge is the contiguous fallthrough from that compare. The canonical
  // validated stitched relation carries every non-sequential in-edge (taken
  // branches, jmp/call targets, resolved indirect candidates, stitched
  // continuations) but never a plain sequential fallthrough, and the compare is
  // never a branch instruction, so ANY canonical edge whose destination is the
  // branch address is a second, compare-bypassing in-edge -> attach nothing
  // (fail-closed). If the predecessor set is ambiguous we likewise attach
  // nothing.
  std::set<Address> canonical_edge_destinations;
  for (const auto &edge : canonical_edges) {
    switch (edge.kind) {
    case M68kStaticEdgeKind::direct_call:
    case M68kStaticEdgeKind::indirect_call:
      canonical_edge_destinations.insert(edge.call ? edge.call->callee.value : edge.target.value);
      break;
    default:
      canonical_edge_destinations.insert(edge.target.value);
      break;
    }
  }
  for (const auto &[address, decoded] : entries) {
    const bool is_cond_branch =
        decoded.kind == M68kInstructionKind::bne_short ||
        (decoded.kind == M68kInstructionKind::branch && decoded.condition != M68kCondition::always &&
         decoded.condition != M68kCondition::never);
    if (!is_cond_branch) continue;
    // F1: a canonical in-edge to the branch means the compare did not
    // necessarily execute on that path -> unsound narrowing. Fail closed.
    if (canonical_edge_destinations.count(address) != 0U) continue;
    const auto pred_it = fallthrough_pred.find(address);
    if (pred_it == fallthrough_pred.end()) continue;
    const auto compare_it = entries.find(pred_it->second);
    if (compare_it == entries.end()) continue;
    std::uint8_t reg = 0U;
    std::optional<M68kValueFlowSuccessor::DnConstraint> taken_c;
    std::optional<M68kValueFlowSuccessor::DnConstraint> nt_c;
    if (!m68k_derive_unsigned_compare_branch_constraints(compare_it->second, decoded, reg, taken_c, nt_c))
      continue;
    const auto next_pc = static_cast<Address>(address + decoded.provenance.length.value);
    auto adj_it = adjacency.find(address);
    if (adj_it == adjacency.end()) continue;
    for (auto &succ : adj_it->second) {
      if (succ.force_unknown) continue;
      if (succ.address == next_pc) {
        if (nt_c && !succ.dn_constraint) succ.dn_constraint = nt_c;
      } else {
        if (taken_c && !succ.dn_constraint) succ.dn_constraint = taken_c;
      }
    }
  }
  return adjacency;
}

// SEG-007-T178 / ADR-0009 producer extension: the bounded callee register-
// write footprint proof. `exhaustive_unknown` forces the caller to fall back
// to the existing whole-state-unknown call continuation.
struct M68kRegisterFootprint {
  std::array<bool, 8> dn_written{};
  std::array<bool, 8> an_written{};
  bool exhaustive_unknown{false};
};

// Walk the already-decoded instructions reachable from callee entry `entry`
// over `entries` (the same decode cache the return-edge machinery uses),
// following non-`force_unknown` value-flow successors, stopping at rts/rte,
// bounded by a per-visit set and by `depth_budget` (seeded from the existing
// `max_call_frame_depth` ceiling -- never a new unbounded walk). Returns the
// union of `m68k_written_data_registers` / `m68k_written_address_registers`
// over that set, recursing into a nested JSR/BSR callee's own footprint. Any
// missing decode, unknown nested callee, or exhausted depth budget yields
// `exhaustive_unknown = true`. Memoized per callee entry within one analysis.
// SEG-007-T188 / ADR-0029: successors come from the shared canonical control
// adjacency (`adjacency`), so the footprint walk still traverses taken
// branches / jmp targets / resolved indirect candidates inside the callee body
// exactly as the pre-T188 self-computed value-flow graph did -- those
// transfers now originate from the canonical validated stitched edge relation
// instead of a second private reconstruction.
M68kRegisterFootprint m68k_compute_callee_register_footprint(
    Address entry, const std::map<Address, M68kDecodedInstruction> &entries,
    const std::map<Address, std::vector<M68kValueFlowSuccessor>> &adjacency, unsigned depth_budget,
    std::map<Address, M68kRegisterFootprint> &memo) {
  if (const auto it = memo.find(entry); it != memo.end()) return it->second;
  if (depth_budget == 0U) {
    M68kRegisterFootprint exhausted{};
    exhausted.exhaustive_unknown = true;
    memo[entry] = exhausted;
    return exhausted;
  }
  // Conservative in-progress sentinel: a recursive callee (A -> ... -> A) sees
  // `exhaustive_unknown` for the self-reference until the real result lands.
  {
    M68kRegisterFootprint sentinel{};
    sentinel.exhaustive_unknown = true;
    memo[entry] = sentinel;
  }
  M68kRegisterFootprint result{};
  std::set<Address> visited;
  std::deque<Address> work{entry};
  while (!work.empty()) {
    const auto current = work.front();
    work.pop_front();
    if (!visited.insert(current).second) continue;
    const auto found = entries.find(current);
    if (found == entries.end()) {
      result.exhaustive_unknown = true;
      continue;
    }
    const auto &decoded = found->second;
    for (const auto reg : m68k_written_data_registers(decoded)) result.dn_written[reg] = true;
    for (const auto reg : m68k_written_address_registers(decoded)) result.an_written[reg] = true;
    const bool is_nested_call =
        decoded.kind == M68kInstructionKind::jsr || decoded.kind == M68kInstructionKind::bsr;
    const auto adj = adjacency.find(current);
    const std::vector<M68kValueFlowSuccessor> empty_succ;
    for (const auto &succ : (adj != adjacency.end() ? adj->second : empty_succ)) {
      if (is_nested_call) {
        if (succ.force_unknown) {
          // The post-call continuation inside this callee body: keep walking it.
          if (succ.callee_entry) {
            const auto sub = m68k_compute_callee_register_footprint(*succ.callee_entry, entries, adjacency,
                                                                    depth_budget - 1U, memo);
            for (std::size_t i = 0; i < 8U; ++i) {
              result.dn_written[i] = result.dn_written[i] || sub.dn_written[i];
              result.an_written[i] = result.an_written[i] || sub.an_written[i];
            }
            if (sub.exhaustive_unknown) result.exhaustive_unknown = true;
          } else {
            result.exhaustive_unknown = true;  // unknown nested callee
          }
          work.push_back(succ.address);
        }
        // The non-force_unknown callee edge is handled by the recursion above,
        // not walked as part of this body.
        continue;
      }
      if (succ.force_unknown) {
        result.exhaustive_unknown = true;
        continue;
      }
      work.push_back(succ.address);
    }
  }
  memo[entry] = result;
  return result;
}

// SEG-007-T186: select the minimum deterministic set of unknown-state roots
// needed to cover the validated stitched value-flow graph. Offline partition
// entries are candidates, not register facts. An entry already reachable from
// an authoritative root or an earlier source component must not inject an
// artificial unknown predecessor into an interior finite producer path.
std::vector<M68kProgramAddress> m68k_select_stitched_analysis_roots(
    const std::map<Address, M68kDecodedInstruction> &entries,
    const std::vector<M68kProgramAddress> &authoritative_roots,
    const std::vector<M68kProgramAddress> &candidate_roots,
    const std::vector<M68kStaticEdge> &canonical_edges) {
  // SEG-007-T188 / ADR-0029: structural reachability is taken from the shared
  // canonical control adjacency (canonical validated stitched edges unioned
  // with decode-local `next_pc`). return_to_continuation stays excluded there,
  // which keeps this selection's SCC/topology identical for programs with no
  // previously-missing proven indexed-control edge.
  const auto adjacency = m68k_canonical_control_adjacency(entries, canonical_edges);
  std::map<Address, std::vector<Address>> successors;
  std::map<Address, std::vector<Address>> predecessors;
  for (const auto &[address, decoded] : entries) {
    (void)decoded;
    auto &out = successors[address];
    const auto adj = adjacency.find(address);
    if (adj != adjacency.end())
      for (const auto &successor : adj->second) {
        if (!entries.contains(successor.address)) continue;
        out.push_back(successor.address);
        predecessors[successor.address].push_back(address);
      }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
  }
  for (auto &[address, incoming] : predecessors) {
    (void)address;
    std::sort(incoming.begin(), incoming.end());
    incoming.erase(std::unique(incoming.begin(), incoming.end()), incoming.end());
  }

  // Iterative Kosaraju keeps this bounded by the already finite decoded graph
  // without host-stack recursion.
  std::set<Address> visited;
  std::vector<Address> finish_order;
  for (const auto &[start, decoded] : entries) {
    (void)decoded;
    if (!visited.insert(start).second) continue;
    std::vector<std::pair<Address, std::size_t>> stack{{start, 0U}};
    while (!stack.empty()) {
      auto &[node, index] = stack.back();
      const auto &out = successors[node];
      if (index < out.size()) {
        const auto next = out[index++];
        if (visited.insert(next).second) stack.emplace_back(next, 0U);
      } else {
        finish_order.push_back(node);
        stack.pop_back();
      }
    }
  }
  std::map<Address, std::size_t> component_by_address;
  std::size_t component_count = 0U;
  for (auto it = finish_order.rbegin(); it != finish_order.rend(); ++it) {
    if (component_by_address.contains(*it)) continue;
    std::vector<Address> pending{*it};
    component_by_address.emplace(*it, component_count);
    for (std::size_t index = 0; index < pending.size(); ++index)
      for (const auto previous : predecessors[pending[index]])
        if (component_by_address.emplace(previous, component_count).second)
          pending.push_back(previous);
    ++component_count;
  }
  std::vector<std::set<std::size_t>> component_successors(component_count);
  std::vector<std::size_t> indegree(component_count, 0U);
  for (const auto &[address, out] : successors) {
    const auto source_component = component_by_address.at(address);
    for (const auto target : out) {
      const auto target_component = component_by_address.at(target);
      if (source_component != target_component &&
          component_successors[source_component].insert(target_component).second)
        ++indegree[target_component];
    }
  }
  std::set<std::size_t> ready;
  for (std::size_t component = 0; component < component_count; ++component)
    if (indegree[component] == 0U) ready.insert(component);
  std::vector<std::size_t> topological;
  while (!ready.empty()) {
    const auto component = *ready.begin();
    ready.erase(ready.begin());
    topological.push_back(component);
    for (const auto next : component_successors[component])
      if (--indegree[next] == 0U) ready.insert(next);
  }
  std::vector<std::vector<Address>> candidates_by_component(component_count);
  for (const auto &candidate : candidate_roots)
    if (candidate.space == TargetAddressSpace::m68k_program && entries.contains(candidate.value))
      candidates_by_component[component_by_address.at(candidate.value)].push_back(candidate.value);
  for (auto &roots : candidates_by_component) {
    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
  }
  std::vector<bool> covered(component_count, false);
  const auto cover = [&](std::size_t first) {
    std::vector<std::size_t> pending{first};
    for (std::size_t index = 0; index < pending.size(); ++index) {
      const auto component = pending[index];
      if (covered[component]) continue;
      covered[component] = true;
      for (const auto next : component_successors[component]) pending.push_back(next);
    }
  };
  std::vector<M68kProgramAddress> selected = authoritative_roots;
  for (const auto &root : authoritative_roots)
    if (root.space == TargetAddressSpace::m68k_program && entries.contains(root.value))
      cover(component_by_address.at(root.value));
  for (const auto component : topological) {
    if (covered[component] || candidates_by_component[component].empty()) continue;
    selected.push_back({TargetAddressSpace::m68k_program, candidates_by_component[component].front()});
    cover(component);
  }
  return selected;
}

// SEG-007-T151: the reachable-RTS successor rule itself moved to the shared,
// exported `m68k_reachable_return_edges` (static_program.cpp/hpp), which
// `synthesize_return_edges` below now calls directly -- see that shared
// function's own doc comment.

// SEG-014-T003: the CPU-owned production static-graph traversal, moved
// verbatim in structure from the pre-T003 discover_m68k_general_startup's own
// recursive DFS + `switch (decoded.kind)` successor selector
// (src/m68k_pipeline_frontend.cpp). State that used to be captured-by-
// reference lambdas over local variables now lives as members of this
// private helper class instead; the algorithm itself -- traversal order,
// visited-state/edge/frame deduplication identity, and the T062/T064
// best-effort sibling-exploration timing -- is unchanged.
class M68kStaticGraphWalker {
 public:
  M68kStaticGraphWalker(M68kProgramAddress entry, const M68kStaticDiscoveryLimits &limits,
                         M68kStaticDiscoveryEnvironment &environment,
                         const std::set<Address> &independent_unit_boundaries,
                         const std::set<Address> &fallthrough_continuation_boundaries)
      : entry_(entry), limits_(limits), environment_(environment),
        boundaries_(independent_unit_boundaries),
        continuation_boundaries_(fallthrough_continuation_boundaries) {}

  // SEG-007-T180 / ADR-0026: enqueue a statically-resolved control-transfer
  // destination unless it is an independently-validated offline inventory
  // unit boundary (and not this pass's own entry, which a root must always
  // walk). A stitched boundary still had its edge/frame/block-entry recorded
  // by the caller; only the recursive body walk is elided -- that body is
  // owned by the boundary address's own unit. NEVER call this for sequential
  // fallthrough or a call/branch continuation (`next_pc`): those are always
  // pushed directly onto `worklist_`.
  void enqueue_control_target(Address target) {
    if (target != entry_.value && boundaries_.count(target) != 0U) {
      ++stitched_boundary_edges_;
      stitched_boundary_addresses_.insert(target);
      return;
    }
    // SEG-007-T182 / ADR-0028: record every resolved direct-control destination
    // whose body this pass will recursively walk, so a later ceiling trip at
    // that address (reached as a control target, not a `next_pc` step) can be
    // classified as eligible for a Phase-2-synthesized resolved-control-target
    // unit rather than a fatal open-control-edge `discovery_budget_exhausted`.
    control_target_reached_.insert(target);
    worklist_.push_back(target);
  }

  // SEG-007-T181 / ADR-0027: enqueue a `next_pc` continuation (sequential
  // fallthrough, or a branch/call continuation address) unless it is a member
  // of the SEPARATE fallthrough-continuation boundary set (and not this pass's
  // own entry). A stitched continuation still gets its `fallthrough_continuation`
  // edge and its block entry recorded here; only the recursive body walk is
  // elided -- that body is owned by the boundary address's own unit. Every
  // enqueued (non-stitched) `next_pc` is recorded in `next_pc_reached_` so a
  // later ceiling trip at that address can be distinguished from a ceiling trip
  // at a genuine open control edge (ADR-0026 §2 / ADR-0027 §6). An empty set
  // (the default) makes every `next_pc` step byte-identical to the pre-T181
  // walk.
  void enqueue_next_pc(Address next_pc, const InstructionProvenance &from) {
    if (next_pc != entry_.value && continuation_boundaries_.count(next_pc) != 0U) {
      note_block_entry({TargetAddressSpace::m68k_program, next_pc}, from);
      if (continuation_edges_emitted_.insert(from.source.address.value).second) {
        edges_.push_back({from, M68kStaticEdgeKind::fallthrough_continuation,
                          {TargetAddressSpace::m68k_program, next_pc}, std::nullopt});
        ++stitched_fallthrough_continuation_edges_;
      }
      return;
    }
    next_pc_reached_.insert(next_pc);
    worklist_.push_back(next_pc);
  }

  // ADR-0011 Decisions §§1-3, revised by ADR-0014 Decision §1 (M1): an
  // explicit, heap-allocated worklist over bare addresses replaces the
  // pre-ADR-0011 host-recursive `(addr, stack_signature(frame_stack))`-keyed
  // DFS. Every admitted address is popped and processed exactly once
  // (`visited_states_` dedup, checked before processing); a failed item's own
  // failure is classified as this walk's primary issue (the first one
  // encountered, in deterministic drain order) or, once a primary already
  // exists, appended to `secondary_issues_` -- exactly the same
  // first-failure-wins/rest-best-effort shape `walk_indirect_control`'s own
  // multi-candidate loop already used before this task, now applied
  // uniformly to every multi-successor instruction kind.
  //
  // ADR-0014 Decision §1 (M1): the worklist is drained first-in-first-out
  // (breadth-first) rather than last-in-first-out (depth-first). Every
  // address that reaches the admission increment was enqueued by a
  // previously processed address, itself reachable from the seed set through
  // already-admitted addresses -- so the admitted-address set induces a
  // connected control-flow subgraph containing the seed at every point during
  // the walk (ADR-0014 "entry-connected admission" invariant, part of P1).
  // Successors are still enqueued in the existing fixed per-kind order
  // (fallthrough before taken branch; callee before continuation; ADR-0009
  // indirect candidates in their existing sorted order, see
  // `process_indirect_control` below), so draining the queue in enqueue order
  // keeps the admitted set a deterministic pure function of the image --
  // ADR-0011 §3's termination argument (each address processed at most once
  // via `visited_states_`; bounded enqueues per address; the fixed ceiling
  // caps the total) is order-independent and unaffected by this change. The
  // ADR-0013 §2 boundary provenance probe inside `decode_instruction`'s
  // ceiling branch below is unchanged; once `instructions_used_` reaches the
  // ceiling, every still-unvisited worklist entry -- exactly the set of open
  // control-flow edges at the admitted-region frontier, ADR-0014 Decision §2
  // -- independently reaches that same probe path as this drain continues,
  // producing the ceiling-trip address's own primary issue followed by one
  // secondary issue per remaining distinct boundary address, deduplicated by
  // `visited_states_` exactly as before.
  M68kStaticDiscoveryResult run() {
    block_entries_.insert(entry_.value);
    block_entry_order_.push_back(entry_.value);
    worklist_.push_back(entry_.value);
    const auto record_failure = [&] {
      // `failure_` is reused as scratch storage by every internal helper on
      // every item, so the running primary must be moved out into its own
      // slot immediately -- leaving it in `failure_` would let a later
      // item's own failure silently overwrite it.
      if (primary_issue_) {
        if (failure_) secondary_issues_.push_back(std::move(*failure_));
      } else {
        primary_issue_ = std::move(failure_);
      }
      failure_.reset();
    };
    bool did_final_indirect_an_pass = false;
    for (;;) {
      while (!worklist_.empty()) {
        const auto addr = worklist_.front();
        worklist_.pop_front();
        if (!visited_states_.insert(addr).second) continue;  // already processed this address
        if (!process_one(addr)) record_failure();
      }
      if (did_final_indirect_an_pass || deferred_indirect_an_.empty()) break;
      // SEG-007-T178 / ADR-0009 producer extension: every reachable callee
      // body is now decoded; retry the deferred An-indirect sites once,
      // authoritatively. A resolved site enqueues its candidate/continuation
      // work, drained by the next iteration; then the loop terminates.
      did_final_indirect_an_pass = true;
      indirect_an_final_pass_ = true;
      const auto &entries = decode_cache_.entries();
      for (const auto site : deferred_indirect_an_) {
        const auto found = entries.find(site);
        if (found == entries.end()) continue;
        if (!process_indirect_control_an(site, found->second)) record_failure();
      }
    }
    synthesize_return_edges();

    M68kStaticDiscoveryResult result{};
    result.decode_cache = std::move(decode_cache_);
    result.decode_order.reserve(decode_order_.size());
    for (const auto value : decode_order_) result.decode_order.push_back({TargetAddressSpace::m68k_program, value});
    result.block_entries.reserve(block_entry_order_.size());
    for (const auto value : block_entry_order_)
      result.block_entries.push_back({TargetAddressSpace::m68k_program, value});
    result.edges = std::move(edges_);
    result.frames = std::move(frames_);
    result.indirect_target_ea_sets = std::move(indirect_target_ea_sets_);
    result.unproven_indirect_control_ea_sets = std::move(unproven_indirect_control_ea_sets_);
    // SEG-021-T028: final-owner invariant by set comparison only (no second
    // CFG traversal). `indirect_emitted_` holds every source that received a
    // final Tier-1 or Tier-2 owner.
    for (const auto site : tier2_eligible_control_sources_)
      if (indirect_emitted_.find(site) == indirect_emitted_.end())
        result.ownerless_tier2_eligible_control_sources.push_back({TargetAddressSpace::m68k_program, site});
    result.completion_rts = completion_rts_;
    result.primary_issue = std::move(primary_issue_);
    result.secondary_issues = std::move(secondary_issues_);
    result.stitched_boundary_edges = stitched_boundary_edges_;
    result.stitched_boundary_addresses = stitched_boundary_addresses_;
    result.stitched_fallthrough_continuation_edges = stitched_fallthrough_continuation_edges_;
    result.fallthrough_continuation_frontier.reserve(fallthrough_continuation_frontier_.size());
    for (const auto value : fallthrough_continuation_frontier_)
      result.fallthrough_continuation_frontier.push_back({TargetAddressSpace::m68k_program, value});
    result.resolved_control_target_frontier.reserve(resolved_control_target_frontier_.size());
    for (const auto value : resolved_control_target_frontier_)
      result.resolved_control_target_frontier.push_back({TargetAddressSpace::m68k_program, value});
    return result;
  }

 private:
  // Common rejection shape shared by every direct-target/discovery-time
  // failure that carries a fully-known instruction provenance: the
  // provenance-paired image offset (see M68kDiscoveryIssue::
  // set_pc_based_image_offset's own doc comment for the sole exception), and
  // an optional candidate target address. This single shape reproduces both
  // the pre-T003 monolith's own `target_rejection` helper AND its
  // `build_static_call_target_rejection` helper -- the two produced
  // byte-for-byte identical FrontendRejected field shapes (verified against
  // the pre-T003 source), differing only in which category/target value the
  // caller supplied, so they share this one CPU-side construction here.
  [[nodiscard]] static M68kDiscoveryIssue provenance_issue(DirectFlowDiagnostic category,
                                                            const InstructionProvenance &source_provenance,
                                                            std::optional<Address> target = std::nullopt) {
    M68kDiscoveryIssue issue{};
    issue.category = category;
    issue.address = source_provenance.source.address;
    issue.provenance = source_provenance;
    issue.instruction_length = source_provenance.length.value;
    if (target) issue.target = M68kProgramAddress{TargetAddressSpace::m68k_program, *target};
    return issue;
  }

  // Block-entry registration helper. ADR-0010 §2 / ADR-0013 (this task's
  // binding scope item 2): `m68k_discovery_max_blocks` is retired as an
  // independent discovery admission gate -- a block-entry count can no
  // longer refuse a block entry or raise a `discovery_budget_exhausted`
  // failure. `m68k_discovery_max_instructions` remains the single resource
  // ceiling (enforced in `decode_instruction`). Every kind of reference that
  // can newly recognize an address as a block entry -- a branch
  // fallthrough/taken target, a JSR/BSR callee or continuation target, an
  // indirect candidate target, or a merge into an already-decoded address
  // reached via a different path -- goes through this one helper.
  void note_block_entry(M68kProgramAddress addr, const InstructionProvenance &) {
    if (block_entries_.contains(addr.value)) return;
    block_entries_.insert(addr.value);
    block_entry_order_.push_back(addr.value);
  }

  // Mirrors the pre-T003 monolith's own check_target: odd, then
  // environment-admitted mapping (unmapped/conflicting), then a
  // mid_instruction_direct_target check using this CPU boundary's own
  // canonical decode map -- the sole structural-interval authority; there is
  // no second interval list. Scoped to direct branch targets only
  // (bne_short/bra_short/the generalized `branch`/`dbcc` kinds); a direct
  // call target (JMP/JSR/BSR) has never had this check and must not gain one
  // here (see M68kDiscoveryTargetRole's own doc comment).
  [[nodiscard]] bool validate_branch_target(Address target, const InstructionProvenance &source_provenance) {
    if ((target & 1U) != 0U) {
      failure_ = provenance_issue(DirectFlowDiagnostic::odd_direct_target, source_provenance, target);
      return false;
    }
    if (const auto mapping_issue =
            environment_.admit_target({TargetAddressSpace::m68k_program, target}, M68kDiscoveryTargetRole::direct_branch)) {
      auto issue = provenance_issue(mapping_issue->category, source_provenance, target);
      if (mapping_issue->category == DirectFlowDiagnostic::conflicting_address_mapping)
        issue.mapping_claims = mapping_issue->matched_claims;
      failure_ = std::move(issue);
      return false;
    }
    const auto &entries = decode_cache_.entries();
    if (const auto it = entries.upper_bound(target); it != entries.begin()) {
      const auto prev = std::prev(it);
      const auto begin = prev->first;
      const auto end = static_cast<std::uint64_t>(begin) + prev->second.provenance.length.value;
      if (target > begin && static_cast<std::uint64_t>(target) < end) {
        failure_ = provenance_issue(DirectFlowDiagnostic::mid_instruction_direct_target, source_provenance, target);
        return false;
      }
    }
    accepted_branch_targets_.emplace_back(target, source_provenance);
    return true;
  }

  // The CPU-local per-operand resolution helper (spec: "access()"). No-ops
  // for a non-statically-foldable EA exactly like every other selected
  // kind's runtime-only operand positions; otherwise canonicalizes and
  // alignment-checks the address itself (both pure MC68000 addressing-mode
  // mechanics, already CPU-owned) before ever asking the environment to
  // classify the access. Retains the resolved request for the caller to
  // attach to a resulting issue's `access` field whenever the environment
  // rejects it, uniformly across every rejection category -- which
  // categories are worth surfacing an access shape for is a scenario-side
  // reporting decision (see M68kDiscoveryIssue::access's own doc comment),
  // not one this CPU boundary makes.
  [[nodiscard]] std::optional<DirectFlowDiagnostic> resolve_operand(const M68kEffectiveAddress &ea,
                                                                      M68kMemoryAccessWidth width,
                                                                      M68kMemoryAccessDirection direction,
                                                                      const InstructionProvenance &provenance) {
    if (!m68k_is_statically_foldable_control_ea(ea)) return std::nullopt;
    const auto address = m68k_canonical_ea_address(ea);
    if (const auto misaligned = m68k_startup_absolute_operand_alignment(address, width)) return *misaligned;
    const M68kCpuMemoryAccessRequest request{{TargetAddressSpace::m68k_program, address}, width, direction,
                                              provenance};
    const auto diagnostic = environment_.classify_memory_access(request);
    if (diagnostic) pending_access_ = request;
    return diagnostic;
  }

  // Builds the reject_selected-equivalent issue for a resolved operand
  // failure and records it as this walk's failure. `pc` is the failing
  // instruction's own address (not the operand's target, which is `target`).
  [[nodiscard]] std::optional<M68kDecodedInstruction> reject_operand(Address pc,
                                                                       const M68kDecodedInstruction &decoded,
                                                                       DirectFlowDiagnostic category,
                                                                       std::uint32_t target) {
    (void)pc;
    auto issue = provenance_issue(category, decoded.provenance, target);
    issue.reconstruct_single_mapping_claim = true;
    issue.reconstruct_instruction_read_access = true;
    if (pending_access_) {
      issue.access = pending_access_;
      pending_access_.reset();
    }
    failure_ = std::move(issue);
    return std::nullopt;
  }

  // Instruction-decode helper: bounded by
  // M68kStaticDiscoveryLimits::max_instructions, checked before any decode
  // attempt is made so an exhausted budget never consumes one. Reuses the
  // shared M68kStaticDecodeCache/decode_m68k_instruction boundary and the
  // ENTIRE per-`decoded.kind` operand read/write validation switch the
  // pre-T003 monolith's own `decode_instruction` owned.
  [[nodiscard]] std::optional<M68kDecodedInstruction> decode_instruction(Address pc_value) {
    const M68kProgramAddress pc{TargetAddressSpace::m68k_program, pc_value};
    pending_access_.reset();
    if (instructions_used_ == limits_.max_instructions) {
      // ADR-0013 Decision §2: exactly one side-effect-free boundary
      // provenance probe on a ceiling trip. Resolve the instruction source
      // and decode through the pure decode_m68k_instruction entry point with
      // the SAME profile the canonical discovery decode uses
      // (general_startup) -- never a narrower probe-specific profile -- and
      // replicate decode_or_get's own image-offset repair
      // (`accepted.provenance.source.image_offset = source.
      // provenance_image_offset;`). The probe never touches decode_cache_,
      // decode_order_, block_entries_, block_entry_order_,
      // instructions_used_, visited_states_, pending_access_, or
      // accepted_branch_targets_, and never runs the per-kind operand
      // read/write validation switch below.
      const auto probe_source_result = environment_.instruction_source(pc);
      if (const auto *probe_source = std::get_if<M68kInstructionSource>(&probe_source_result)) {
        const auto probe_decode = decode_m68k_instruction(probe_source->bytes, probe_source->source,
                                                            M68kDecodeProfile::general_startup);
        if (const auto *probe_accepted = std::get_if<M68kDecodedInstruction>(&probe_decode)) {
          auto probed_provenance = probe_accepted->provenance;
          probed_provenance.source.image_offset = probe_source->provenance_image_offset;
          // Decision §3: no `target`, no `unresolved_reason` -- the empty
          // `unresolved_reason` + `has_target == false` combination is the
          // boundary's own classification discriminator.
          auto issue = provenance_issue(DirectFlowDiagnostic::discovery_budget_exhausted, probed_provenance);
          issue.set_pc_based_image_offset = true;
          issue.reconstruct_single_mapping_claim = true;
          issue.reconstruct_instruction_read_access = true;
          // SEG-007-T181 / ADR-0027 §1/§6: the probe decoded cleanly as a
          // prefix-boundary shape (no open control target, empty unresolved
          // reason). If this ceiling-trip address was reached by a `next_pc`
          // step (sequential fallthrough or branch/call continuation), it is
          // eligible for a Phase-2-synthesized fallthrough-continuation unit.
          // A ceiling trip at a genuine open control edge is never in
          // `next_pc_reached_` and keeps its existing fatal diagnostic only.
          if (next_pc_reached_.count(pc_value) != 0U &&
              std::find(fallthrough_continuation_frontier_.begin(),
                        fallthrough_continuation_frontier_.end(),
                        pc_value) == fallthrough_continuation_frontier_.end())
            fallthrough_continuation_frontier_.push_back(pc_value);
          // SEG-007-T182 / ADR-0028: otherwise, if the ceiling-trip address was
          // reached only as a statically-resolved+validated direct
          // control-transfer destination, it is eligible for a
          // Phase-2-synthesized resolved-control-target unit. A ceiling trip at
          // a genuinely unresolved open control edge is in neither set and keeps
          // its fatal diagnostic.
          else if (next_pc_reached_.count(pc_value) == 0U &&
                   control_target_reached_.count(pc_value) != 0U &&
                   std::find(resolved_control_target_frontier_.begin(),
                             resolved_control_target_frontier_.end(),
                             pc_value) == resolved_control_target_frontier_.end())
            resolved_control_target_frontier_.push_back(pc_value);
          failure_ = std::move(issue);
          return std::nullopt;
        }
      }
      // On ANY probe failure -- unmapped or conflicting instruction source,
      // truncated, illegal, unsupported form, or a decoded length exceeding
      // the covering claim's remaining bytes (already enforced by
      // `probe_source->bytes`'s own bound) -- no boundary is constructed:
      // keep today's whole-program rejection. Provenance is never
      // fabricated.
      M68kDiscoveryIssue issue{};
      issue.category = DirectFlowDiagnostic::discovery_budget_exhausted;
      issue.address = pc;
      issue.unresolved_reason = "m68k_discovery_max_instructions";
      failure_ = std::move(issue);
      return std::nullopt;
    }
    const auto source_result = environment_.instruction_source(pc);
    if (const auto *source_issue = std::get_if<M68kInstructionSourceIssue>(&source_result)) {
      M68kDiscoveryIssue issue{};
      issue.category = source_issue->kind == M68kInstructionSourceIssueKind::unmapped
                            ? DirectFlowDiagnostic::unmapped_instruction_address
                            : DirectFlowDiagnostic::conflicting_address_mapping;
      issue.address = pc;
      issue.mapping_claims = source_issue->matched_claims;
      failure_ = std::move(issue);
      return std::nullopt;
    }
    const auto &source = std::get<M68kInstructionSource>(source_result);
    const auto decode_result = decode_cache_.decode_or_get(source, M68kDecodeProfile::general_startup);
    if (const auto *bad = std::get_if<M68kDiscoveryDecodeIssue>(&decode_result)) {
      M68kDiscoveryIssue issue{};
      issue.category = bad->kind == M68kDiscoveryDecodeIssueKind::truncated_instruction
                            ? DirectFlowDiagnostic::truncated_instruction
                        : bad->kind == M68kDiscoveryDecodeIssueKind::illegal_instruction
                            ? DirectFlowDiagnostic::illegal_instruction
                        : bad->kind == M68kDiscoveryDecodeIssueKind::unsupported_instruction_form
                            ? DirectFlowDiagnostic::unsupported_instruction_form
                            : DirectFlowDiagnostic::valid_but_unsupported_instruction;
      issue.address = pc;
      issue.set_pc_based_image_offset = true;
      issue.reconstruct_single_mapping_claim = true;
      if (issue.category == DirectFlowDiagnostic::truncated_instruction) {
        issue.available_bytes = bad->available_bytes;
        issue.requested_length = bad->requested_length;
      }
      if (bad->instruction_length) issue.instruction_length = bad->instruction_length;
      if (bad->provenance) {
        issue.provenance = bad->provenance;
        issue.reconstruct_instruction_read_access = true;
      }
      failure_ = std::move(issue);
      return std::nullopt;
    }
    auto decoded = std::get<M68kDecodedInstruction>(decode_result);
    const auto length = static_cast<std::size_t>(decoded.provenance.length.value);
    const auto local = static_cast<std::size_t>(source.source.image_offset.value);
    if (length > source.bytes.size() - local) {
      M68kDiscoveryIssue issue{};
      issue.category = DirectFlowDiagnostic::truncated_instruction;
      issue.address = pc;
      issue.reconstruct_single_mapping_claim = true;
      failure_ = std::move(issue);
      return std::nullopt;
    }

    // The complete SEG-007-T023/T025 selected-kind operand read/write
    // validation switch, moved verbatim in structure. Every statically-
    // foldable operand is routed through the shared `resolve_operand` above;
    // a runtime-only or register operand position no-ops there exactly as it
    // did in the pre-T003 monolith.
    if (decoded.kind == M68kInstructionKind::move) {
      if (const auto diagnostic = resolve_operand(decoded.source_ea, decoded.size,
                                                    M68kMemoryAccessDirection::read, decoded.provenance))
        return reject_operand(pc_value, decoded, *diagnostic, decoded.source_ea.absolute_address);
      if (const auto diagnostic = resolve_operand(decoded.destination_ea, decoded.size,
                                                    M68kMemoryAccessDirection::write, decoded.provenance))
        return reject_operand(pc_value, decoded, *diagnostic, decoded.destination_ea.absolute_address);
    } else if (decoded.kind == M68kInstructionKind::tst || decoded.kind == M68kInstructionKind::cmp ||
                decoded.kind == M68kInstructionKind::cmpi || decoded.kind == M68kInstructionKind::cmpa ||
                // SEG-021-T018: MOVE <ea>,SR / MOVE <ea>,CCR read one word source.
                decoded.kind == M68kInstructionKind::move_to_sr || decoded.kind == M68kInstructionKind::move_to_ccr ||
                // SEG-021-T019: CHK.W reads its word bound.
                decoded.kind == M68kInstructionKind::chk) {
      if (const auto diagnostic = resolve_operand(decoded.source_ea, decoded.size,
                                                    M68kMemoryAccessDirection::read, decoded.provenance))
        return reject_operand(pc_value, decoded, *diagnostic, decoded.source_ea.absolute_address);
      if (decoded.kind == M68kInstructionKind::cmpi) {
        if (const auto diagnostic = resolve_operand(decoded.destination_ea, decoded.size,
                                                      M68kMemoryAccessDirection::read, decoded.provenance))
          return reject_operand(pc_value, decoded, *diagnostic, decoded.destination_ea.absolute_address);
      }
    } else if (decoded.kind == M68kInstructionKind::sub || decoded.kind == M68kInstructionKind::suba ||
                decoded.kind == M68kInstructionKind::subi || decoded.kind == M68kInstructionKind::subq ||
                decoded.kind == M68kInstructionKind::add || decoded.kind == M68kInstructionKind::adda ||
                 decoded.kind == M68kInstructionKind::addi || decoded.kind == M68kInstructionKind::addq) {
      if (const auto diagnostic = resolve_operand(decoded.source_ea, decoded.size,
                                                    M68kMemoryAccessDirection::read, decoded.provenance))
        return reject_operand(pc_value, decoded, *diagnostic, decoded.source_ea.absolute_address);
      if (decoded.destination_ea.mode != M68kEaMode::data_register &&
          decoded.destination_ea.mode != M68kEaMode::address_register) {
        if (const auto diagnostic = resolve_operand(decoded.destination_ea, decoded.size,
                                                      M68kMemoryAccessDirection::read, decoded.provenance))
          return reject_operand(pc_value, decoded, *diagnostic, decoded.destination_ea.absolute_address);
        if (const auto diagnostic = resolve_operand(decoded.destination_ea, decoded.size,
                                                      M68kMemoryAccessDirection::write, decoded.provenance))
          return reject_operand(pc_value, decoded, *diagnostic, decoded.destination_ea.absolute_address);
      }
    } else if (decoded.kind == M68kInstructionKind::logical_and || decoded.kind == M68kInstructionKind::andi ||
               decoded.kind == M68kInstructionKind::logical_or || decoded.kind == M68kInstructionKind::ori ||
               decoded.kind == M68kInstructionKind::eor || decoded.kind == M68kInstructionKind::eori) {
      if (const auto diagnostic = resolve_operand(decoded.source_ea, decoded.size,
                                                    M68kMemoryAccessDirection::read, decoded.provenance))
        return reject_operand(pc_value, decoded, *diagnostic, decoded.source_ea.absolute_address);
      if (decoded.destination_ea.mode != M68kEaMode::data_register) {
        if (const auto diagnostic = resolve_operand(decoded.destination_ea, decoded.size,
                                                      M68kMemoryAccessDirection::read, decoded.provenance))
          return reject_operand(pc_value, decoded, *diagnostic, decoded.destination_ea.absolute_address);
        if (const auto diagnostic = resolve_operand(decoded.destination_ea, decoded.size,
                                                      M68kMemoryAccessDirection::write, decoded.provenance))
          return reject_operand(pc_value, decoded, *diagnostic, decoded.destination_ea.absolute_address);
      }
    } else if (decoded.kind == M68kInstructionKind::move || decoded.kind == M68kInstructionKind::movea) {
      if (const auto diagnostic = resolve_operand(decoded.source_ea, decoded.size,
                                                    M68kMemoryAccessDirection::read, decoded.provenance))
        return reject_operand(pc_value, decoded, *diagnostic, decoded.source_ea.absolute_address);
      if (decoded.kind == M68kInstructionKind::move) {
        if (const auto diagnostic = resolve_operand(decoded.destination_ea, decoded.size,
                                                      M68kMemoryAccessDirection::write, decoded.provenance))
          return reject_operand(pc_value, decoded, *diagnostic, decoded.destination_ea.absolute_address);
      }
    } else if (decoded.kind == M68kInstructionKind::clr || decoded.kind == M68kInstructionKind::not_operand ||
               decoded.kind == M68kInstructionKind::negate_word ||
               decoded.kind == M68kInstructionKind::negate_extended ||
               decoded.kind == M68kInstructionKind::negate_decimal ||
               decoded.kind == M68kInstructionKind::test_and_set ||
               decoded.kind == M68kInstructionKind::set_conditional ||
               decoded.kind == M68kInstructionKind::move_from_sr) {
      // SEG-021-T029: a memory CLR destination is read (value discarded) before it is written (68000), like
      // memory Scc; a Dn destination is not statically foldable, so resolve_operand ignores it.
      // SEG-021-T018: a memory MOVE from SR destination is read before it is written (68000), like memory Scc.
      // SEG-021-T016: TAS is a byte one-address RMW like NOT; memory Scc is read before it is written (68000).
      // SEG-021-T014: NEG/NEGX share NOT's one-address read-modify-write operand contract.
      // SEG-007-T168: NOT is a genuine one-address read-modify-write,
      // exactly like shift_rotate's memory form below.
      if (const auto diagnostic = resolve_operand(decoded.destination_ea, decoded.size,
                                                    M68kMemoryAccessDirection::read, decoded.provenance))
        return reject_operand(pc_value, decoded, *diagnostic, decoded.destination_ea.absolute_address);
      if (const auto diagnostic = resolve_operand(decoded.destination_ea, decoded.size,
                                                    M68kMemoryAccessDirection::write, decoded.provenance))
        return reject_operand(pc_value, decoded, *diagnostic, decoded.destination_ea.absolute_address);
    } else if (decoded.kind == M68kInstructionKind::btst) {
      if (const auto diagnostic = resolve_operand(decoded.destination_ea, decoded.size,
                                                    M68kMemoryAccessDirection::read, decoded.provenance))
        return reject_operand(pc_value, decoded, *diagnostic, decoded.destination_ea.absolute_address);
    } else if (decoded.kind == M68kInstructionKind::bchg || decoded.kind == M68kInstructionKind::bclr ||
               decoded.kind == M68kInstructionKind::bset) {
      if (const auto diagnostic = resolve_operand(decoded.destination_ea, decoded.size,
                                                    M68kMemoryAccessDirection::read, decoded.provenance))
        return reject_operand(pc_value, decoded, *diagnostic, decoded.destination_ea.absolute_address);
      if (const auto diagnostic = resolve_operand(decoded.destination_ea, decoded.size,
                                                    M68kMemoryAccessDirection::write, decoded.provenance))
        return reject_operand(pc_value, decoded, *diagnostic, decoded.destination_ea.absolute_address);
    } else if (decoded.kind == M68kInstructionKind::movem) {
      const auto &memory_ea = decoded.movem_direction == M68kMovemDirection::registers_to_memory
                                    ? decoded.destination_ea : decoded.source_ea;
      const auto direction = decoded.movem_direction == M68kMovemDirection::registers_to_memory
                                    ? M68kMemoryAccessDirection::write : M68kMemoryAccessDirection::read;
      const auto order = m68k_movem_transfer_order(decoded.movem_register_mask,
          memory_ea.mode == M68kEaMode::address_predec ? M68kMovemTransferOrder::predecrement
                                                        : M68kMovemTransferOrder::ascending);
      const auto width = static_cast<std::uint32_t>(decoded.size);
      for (std::size_t slot = 0; slot < order.size(); ++slot) {
        auto transfer_ea = memory_ea;
        transfer_ea.absolute_address =
            memory_ea.absolute_address + static_cast<std::uint32_t>(slot) * width;
        if (const auto diagnostic = resolve_operand(transfer_ea, decoded.size, direction, decoded.provenance))
          return reject_operand(pc_value, decoded, *diagnostic, transfer_ea.absolute_address);
      }
    } else if (decoded.kind == M68kInstructionKind::shift_rotate) {
      if (const auto diagnostic = resolve_operand(decoded.destination_ea, decoded.size,
                                                    M68kMemoryAccessDirection::read, decoded.provenance))
        return reject_operand(pc_value, decoded, *diagnostic, decoded.destination_ea.absolute_address);
      if (const auto diagnostic = resolve_operand(decoded.destination_ea, decoded.size,
                                                    M68kMemoryAccessDirection::write, decoded.provenance))
        return reject_operand(pc_value, decoded, *diagnostic, decoded.destination_ea.absolute_address);
    }

    // Order-independent mid_instruction_direct_target detection (reverse
    // direction): this instruction's own newly-verified span must not
    // strictly contain any direct-branch target already accepted earlier in
    // this discovery pass.
    for (const auto &accepted : accepted_branch_targets_) {
      const auto accepted_target = accepted.first;
      const auto begin = static_cast<std::uint64_t>(pc_value);
      const auto end = begin + length;
      if (static_cast<std::uint64_t>(accepted_target) > begin &&
          static_cast<std::uint64_t>(accepted_target) < end) {
        failure_ = provenance_issue(DirectFlowDiagnostic::mid_instruction_direct_target, accepted.second,
                                     accepted_target);
        return std::nullopt;
      }
    }
    ++instructions_used_;
    return decoded;
  }

  // ADR-0011 Decisions §§1-3: processes exactly one already-popped, not-yet-
  // visited address. Every control-flow path (branch target, JSR/BSR callee
  // target, indirect candidate, straight-line successor) either decodes a
  // not-yet-seen address exactly once (via `decode_instruction`) or reuses an
  // already-canonicalized decode. Zero, one, or two further addresses are
  // pushed onto `worklist_` in place of the pre-ADR-0011 recursive `walk`
  // calls -- a mechanical control-flow transformation of the same per-kind
  // switch, not a semantic one (branch/fallthrough push both successors;
  // JSR/BSR push callee and continuation independently, per Decision §1;
  // RTS pushes nothing, per Decision §1; `walk_indirect_control` pushes every
  // admitted candidate plus, for an indirect call, the one shared
  // continuation). Returns false with `failure_` set on any rejection; `run`
  // classifies the first such failure as this walk's primary issue and every
  // later one as a best-effort secondary, exactly mirroring the
  // T062/T064 sibling-exploration shape `walk_indirect_control`'s own
  // multi-candidate loop already used before this task.
  bool process_one(Address addr) {
    const M68kDecodedInstruction *decoded_ptr;
    if (const auto it = decode_cache_.entries().find(addr); it != decode_cache_.entries().end()) {
      decoded_ptr = &it->second;  // canonical decode reused under a new reference path
    } else {
      const auto decoded_opt = decode_instruction(addr);
      if (!decoded_opt) return false;
      decoded_ptr = &decode_cache_.entries().at(addr);
      decode_order_.push_back(addr);
    }
    const auto &decoded = *decoded_ptr;
    const auto next_pc_value = static_cast<Address>(addr + decoded.provenance.length.value);

    switch (decoded.kind) {
    case M68kInstructionKind::bne_short: {
      const auto taken_value = m68k_branch_target(addr, decoded.operand);
      if (!validate_branch_target(next_pc_value, decoded.provenance)) return false;
      if (!validate_branch_target(taken_value, decoded.provenance)) return false;
      const M68kProgramAddress fallthrough{TargetAddressSpace::m68k_program, next_pc_value};
      const M68kProgramAddress taken{TargetAddressSpace::m68k_program, taken_value};
      note_block_entry(fallthrough, decoded.provenance);
      note_block_entry(taken, decoded.provenance);
      if (branch_edges_emitted_.insert(addr).second) {
        edges_.push_back({decoded.provenance, M68kStaticEdgeKind::fallthrough, fallthrough, std::nullopt});
        edges_.push_back({decoded.provenance, M68kStaticEdgeKind::direct_branch, taken, std::nullopt});
      }
      // ADR-0014 Decision §1: fallthrough enqueued before the taken branch so
      // a first-in-first-out drain processes it first, preserving the
      // established "fallthrough before taken branch" successor order.
      enqueue_next_pc(next_pc_value, decoded.provenance);  // SEG-007-T181: stitch if a continuation boundary
      enqueue_control_target(taken_value);  // SEG-007-T180: stitch if a unit boundary
      return true;
    }
    case M68kInstructionKind::bra_short: {
      const auto taken_value = m68k_branch_target(addr, decoded.operand);
      if (!validate_branch_target(taken_value, decoded.provenance)) return false;
      const M68kProgramAddress taken{TargetAddressSpace::m68k_program, taken_value};
      note_block_entry(taken, decoded.provenance);
      if (branch_edges_emitted_.insert(addr).second) {
        edges_.push_back({decoded.provenance, M68kStaticEdgeKind::direct_branch, taken, std::nullopt});
      }
      enqueue_control_target(taken_value);  // SEG-007-T180: stitch if a unit boundary
      return true;
    }
    case M68kInstructionKind::branch: {
      const auto raw = decoded.source_ea.immediate_value;
      const auto signed_disp = decoded.size == M68kMemoryAccessWidth::byte
                                    ? static_cast<std::int32_t>(static_cast<std::int8_t>(raw))
                                    : static_cast<std::int32_t>(static_cast<std::int16_t>(raw));
      const auto taken_value = m68k_branch_target(addr, signed_disp);
      if (!validate_branch_target(taken_value, decoded.provenance)) return false;
      const M68kProgramAddress taken{TargetAddressSpace::m68k_program, taken_value};
      note_block_entry(taken, decoded.provenance);
      if (decoded.condition == M68kCondition::always) {
        if (branch_edges_emitted_.insert(addr).second) {
          edges_.push_back({decoded.provenance, M68kStaticEdgeKind::direct_branch, taken, std::nullopt});
        }
        enqueue_control_target(taken_value);  // SEG-007-T180: stitch if a unit boundary
        return true;
      }
      if (!validate_branch_target(next_pc_value, decoded.provenance)) return false;
      const M68kProgramAddress fallthrough{TargetAddressSpace::m68k_program, next_pc_value};
      note_block_entry(fallthrough, decoded.provenance);
      if (branch_edges_emitted_.insert(addr).second) {
        edges_.push_back({decoded.provenance, M68kStaticEdgeKind::fallthrough, fallthrough, std::nullopt});
        edges_.push_back({decoded.provenance, M68kStaticEdgeKind::direct_branch, taken, std::nullopt});
      }
      // ADR-0014 Decision §1: fallthrough before taken branch, as above.
      enqueue_next_pc(next_pc_value, decoded.provenance);  // SEG-007-T181: stitch if a continuation boundary
      enqueue_control_target(taken_value);  // SEG-007-T180: stitch if a unit boundary
      return true;
    }
    case M68kInstructionKind::rts: {
      // ADR-0011 Decision §1: RTS no longer resolves, walks, or recurses
      // into any target -- the walk path through an RTS simply ends here.
      // Its return target is a generated-runtime fact (the literal popped
      // SSP value), never a statically-simulated one; the proof obligation
      // ("which addresses may this return legally land on") is satisfied by
      // `synthesize_return_edges` below and the runtime membership check it
      // feeds, not by this traversal. A non-completion RTS therefore neither
      // fails nor schedules further work.
      if (environment_.is_completion_rts(decoded.provenance)) completion_rts_ = decoded.provenance;
      return true;
    }
    case M68kInstructionKind::rte:
    // SEG-021-T019: RTR (the PC popped from its CCR/PC frame) and the instruction-word exceptions (ILLEGAL,
    // line 1010/1111, every other illegal word: control always enters the build-time-rooted vector handler with
    // THIS instruction stacked) have no static successor either.
    case M68kInstructionKind::rtr:
    case M68kInstructionKind::instruction_exception:
      // SEG-007-T047 / ADR-0020 §9: RTE terminates the static walk with no
      // successors, exactly like RTS. The restored PC is a generated-runtime
      // fact (the popped exception-frame PC), never a statically-simulated one.
      return true;
    case M68kInstructionKind::jmp:
    case M68kInstructionKind::jsr: {
      const auto &ea = decoded.source_ea;
      if (!m68k_is_statically_foldable_control_ea(ea)) {
        // SEG-007-T124 / ADR-0009: the one bounded computed/indirect
        // control-EA class this project represents with a Tier-1 finite-
        // value proof -- a brief PC-relative indexed JMP/JSR target proven
        // by the finite-index-value producer. Every other non-foldable form
        // (d16(An), full-format/memory-indirect, long-indexed) keeps the
        // existing fail-closed `reached_unresolved_direct_edge` below
        // unchanged.
        if (ea.mode == M68kEaMode::pc_index8) return process_indirect_control(addr, decoded);
        // SEG-007-T178 / ADR-0009 producer extension: pure address-register-
        // indirect `JMP (An)` / `JSR (An)` with a generation-time-provable
        // finite An code-address set.
        if (ea.mode == M68kEaMode::address_indirect && ea.displacement == 0 && ea.extension_words == 0U)
          return process_indirect_control_an(addr, decoded);
        // SEG-021-T011: `JMP (d8,An,Xn)` / `JSR (d8,An,Xn)` -- the last
        // Motorola control-addressing mode this project admits for JMP/JSR
        // (`m68k_ea_jsr_jmp_control_modes`). This shape's runtime target
        // depends on BOTH a runtime An base and a runtime Dn/An index, so a
        // Tier-1 finite-value proof for it would need a new cross-product
        // base-An-state x index-Dn/An-state producer this task deliberately
        // does not build (see `process_indirect_control_index8`'s own doc
        // comment). It goes straight to the existing Tier-2/ADR-0024/
        // ADR-0025 generalization instead.
        if (ea.mode == M68kEaMode::address_index8) return process_indirect_control_index8(addr, decoded);
        failure_ = provenance_issue(DirectFlowDiagnostic::reached_unresolved_direct_edge, decoded.provenance);
        return false;
      }
      const auto target_value = m68k_canonical_ea_address(ea);
      // ADR-0011 Decision §1: `m68k_discovery_max_call_frame_depth` is
      // retired as a walker admission gate -- there is no more frame_stack
      // for it to bound.
      if (const auto call_issue = environment_.admit_target({TargetAddressSpace::m68k_program, target_value},
                                                              M68kDiscoveryTargetRole::direct_call)) {
        failure_ = provenance_issue(call_issue->category, decoded.provenance, target_value);
        return false;
      }
      if (decoded.kind == M68kInstructionKind::jmp) {
        const M68kProgramAddress target{TargetAddressSpace::m68k_program, target_value};
        note_block_entry(target, decoded.provenance);
        if (branch_edges_emitted_.insert(addr).second) {
          edges_.push_back({decoded.provenance, M68kStaticEdgeKind::direct_branch, target, std::nullopt});
        }
        enqueue_control_target(target_value);  // SEG-007-T180: stitch if a unit boundary
        return true;
      }
      // ADR-0011 Decision §1: admission independently schedules two further
      // walk targets -- the callee's entry address (proves the subroutine
      // body) and the call's own continuation address (proves "what comes
      // after the call"), both ordinary address-only walk targets, neither
      // depending on discovering a matching RTS.
      const auto call = m68k_make_static_call(decoded, target_value);
      const M68kProgramAddress callee_addr{TargetAddressSpace::m68k_program, call.callee.value};
      note_block_entry(callee_addr, decoded.provenance);
      note_block_entry({TargetAddressSpace::m68k_program, next_pc_value}, decoded.provenance);
      if (calls_emitted_.insert(addr).second) {
        frames_.push_back(M68kStaticFrame{call});
        edges_.push_back(m68k_make_static_call_edge(call));
      }
      enqueue_control_target(call.callee.value);  // SEG-007-T180: stitch if a unit boundary
      enqueue_next_pc(next_pc_value, decoded.provenance);  // SEG-007-T181: call continuation may be a continuation boundary
      return true;
    }
    case M68kInstructionKind::bsr: {
      const auto raw = decoded.source_ea.immediate_value;
      const auto signed_disp = decoded.size == M68kMemoryAccessWidth::byte
                                    ? static_cast<std::int32_t>(static_cast<std::int8_t>(raw))
                                    : static_cast<std::int32_t>(static_cast<std::int16_t>(raw));
      const auto target_value = m68k_branch_target(addr, signed_disp);
      // ADR-0011 Decision §1: no more call-frame-depth admission gate.
      if (const auto call_issue = environment_.admit_target({TargetAddressSpace::m68k_program, target_value},
                                                              M68kDiscoveryTargetRole::direct_call)) {
        failure_ = provenance_issue(call_issue->category, decoded.provenance, target_value);
        return false;
      }
      const auto call = m68k_make_static_call(decoded, target_value);
      const M68kProgramAddress callee_addr{TargetAddressSpace::m68k_program, call.callee.value};
      note_block_entry(callee_addr, decoded.provenance);
      note_block_entry({TargetAddressSpace::m68k_program, next_pc_value}, decoded.provenance);
      if (calls_emitted_.insert(addr).second) {
        frames_.push_back(M68kStaticFrame{call});
        edges_.push_back(m68k_make_static_call_edge(call));
      }
      enqueue_control_target(call.callee.value);  // SEG-007-T180: stitch if a unit boundary
      enqueue_next_pc(next_pc_value, decoded.provenance);  // SEG-007-T181: call continuation may be a continuation boundary
      return true;
    }
    case M68kInstructionKind::dbcc: {
      const auto raw = decoded.source_ea.immediate_value;
      const auto signed_disp = static_cast<std::int32_t>(static_cast<std::int16_t>(raw));
      const auto taken_value = m68k_branch_target(addr, signed_disp);
      if (!validate_branch_target(taken_value, decoded.provenance)) return false;
      if (!validate_branch_target(next_pc_value, decoded.provenance)) return false;
      const M68kProgramAddress taken{TargetAddressSpace::m68k_program, taken_value};
      const M68kProgramAddress fallthrough{TargetAddressSpace::m68k_program, next_pc_value};
      note_block_entry(taken, decoded.provenance);
      note_block_entry(fallthrough, decoded.provenance);
      if (branch_edges_emitted_.insert(addr).second) {
        edges_.push_back({decoded.provenance, M68kStaticEdgeKind::fallthrough, fallthrough, std::nullopt});
        edges_.push_back({decoded.provenance, M68kStaticEdgeKind::direct_branch, taken, std::nullopt});
      }
      // ADR-0014 Decision §1: fallthrough before taken branch, as above.
      enqueue_next_pc(next_pc_value, decoded.provenance);  // SEG-007-T181: stitch if a continuation boundary
      enqueue_control_target(taken_value);  // SEG-007-T180: stitch if a unit boundary
      return true;
    }
    default: {
      // moveq, subq_l_1_d0, and every remaining shared whitelist kind whose
      // successor is a plain straight-line advance. If the successor address
      // is already canonically decoded (a straight-line merge into
      // pre-existing code), it must still be registered as a block entry
      // here: visited_states governs re-visitation, not block-entry
      // registration for a straight-line successor that happens to land on
      // already-decoded code.
      const M68kProgramAddress next_pc{TargetAddressSpace::m68k_program, next_pc_value};
      if (decode_cache_.entries().contains(next_pc_value)) note_block_entry(next_pc, decoded.provenance);
      enqueue_next_pc(next_pc_value, decoded.provenance);  // SEG-007-T181: stitch if a continuation boundary
      return true;
    }
    }
  }

  // ADR-0011 Decision §1's return-target proof, reconstructed without a
  // traversal-time frame_stack: for every discovered call, this records the
  // `M68kStaticEdgeKind::return_to_continuation` edge(s) for every RTS its
  // callee may legitimately exit through -- derived once, after the full
  // walk, from the already-collected `decode_cache_`/`frames_` facts instead
  // of a recursive call-frame stack.
  // SEG-007-T134 correction (ADR-0011 Decision §1): the pre-correction
  // implementation only recognized a callee whose entry decoded straight
  // into its own RTS with no intervening control-transfer instruction --
  // any subroutine with an internal branch before its eventual RTS, or with
  // more than one legal exit RTS, silently lost every return_to_continuation
  // edge for every call to it (a real, observable gap against ADR-0011
  // Decision §1's "whole-program set of every proven continuation" model,
  // and against `runtime_frontier_eligible`'s own reachability requirement
  // that a retained continuation block be provably reachable). This
  // generalizes the reconstruction to a proper bounded reachable-RTS trace
  // per called subroutine, over the already-discovered/decoded control flow
  // (`m68k_return_reachability_successors` above), so every RTS a callee may
  // legitimately exit through -- however many internal branches or distinct
  // exit points it has -- is represented, for every call to that callee
  // (a shared RTS naturally receives one edge per distinct calling frame,
  // exactly as before this correction).
  // SEG-007-T151: the reachable-RTS trace itself is now the shared, exported
  // `m68k_reachable_return_edges` (static_program.cpp/hpp) -- this walk
  // merely feeds it its own local decode cache and frame set. The genesis
  // frontend's multi-seed aggregation (`discover_m68k_general_startup`) runs
  // the identical function again over the cross-seed AGGREGATE decode map
  // after every seed's own walk has merged, recovering any RTS reachable
  // only through a decoded path no single seed's own local walk covered
  // end-to-end. See that function's own doc comment for why a per-seed-local
  // trace alone is insufficient once more than one independently promoted
  // seed contributes decoded instructions for the same callee body.
  void synthesize_return_edges() {
    for (auto &edge : m68k_reachable_return_edges(decode_cache_.entries(), frames_)) edges_.push_back(std::move(edge));
  }

 public:
  // SEG-007-T124 / ADR-0009: the bounded finite forward register-value
  // analysis, run fresh over whatever this discovery pass has already
  // canonically decoded (`decode_cache_`) each time a computed control EA
  // needs a proven index value. Nodes are decoded instruction addresses;
  // `entry_` seeds with "unknown for every Dn" (there is no prior fact at
  // the very first instruction); every other address's IN state is the join
  // of every contributing predecessor the shared canonical control adjacency
  // (`m68k_canonical_control_adjacency`, ADR-0029) produces,
  // with no incoming contribution at all defaulting to "unknown" exactly
  // like the seeded state (never a silently-finite default). This is a
  // standard monotone worklist fixed point: `m68k_merge_dn_state` only ever
  // keeps a set unchanged, grows it, or falls to `unknown`, so it terminates
  // within the bounded (<=256-instruction, <=256-value) state space this
  // discovery pass already enforces elsewhere.
  // SEG-007-T188 / ADR-0029: the intra-walk pc_index8 finite-index proof
  // consumes this walk's in-progress `edges_`. `edges_` grows monotonically
  // during a walk (edges are only ever appended, never removed or rewritten),
  // and every instruction on a direct path to the index producer has already
  // been walked -- so its taken-branch / call / stitched edges are already in
  // `edges_` -- by the time an indirect control site is processed. A later
  // edge append can only add successors, which is monotone toward `unknown`
  // in the ADR-0009 lattice (more joins, finite+unknown=unknown); it never
  // retroactively strengthens an earlier finite proof. The final canonical
  // frontend run (frontend.cpp) re-runs this analysis over the complete
  // aggregated `merged_edges`, which is authoritative.
  [[nodiscard]] std::map<Address, M68kRegisterState> analyze_finite_index_values() const {
    return analyze_finite_register_values(decode_cache_.entries(), {entry_}, edges_,
                                          limits_.max_call_frame_depth, environment_);
  }

  // SEG-007-T189 / ADR-0030: `seeded_root_states` and `out_states_by_address`
  // are additive, default-inert parameters that let the bounded call-context-
  // sensitive proof (`m68k_prove_stitched_an_indirect_targets`) re-invoke this
  // SAME fixed point -- unmodified algorithm, unmodified merge/cap/A7/
  // termination argument -- seeded at a single call-site's own contributed
  // register state instead of the ordinary blank root state, and to read back
  // the per-address post-instruction state the base (context-insensitive)
  // walk itself computed. Every existing caller passes neither, so every
  // pre-T189 call site is byte-for-byte unaffected.
  [[nodiscard]] static std::map<Address, M68kRegisterState> analyze_finite_register_values(
      const std::map<Address, M68kDecodedInstruction> &entries,
      const std::vector<M68kProgramAddress> &entry_roots,
      const std::vector<M68kStaticEdge> &canonical_edges, std::uint32_t max_call_frame_depth,
      M68kStaticDiscoveryEnvironment &environment,
      const std::vector<M68kProgramAddress> &disconnected_root_candidates = {},
      const std::map<Address, M68kRegisterState> *seeded_root_states = nullptr,
      std::map<Address, M68kRegisterState> *out_states_by_address = nullptr) {
    std::map<Address, std::optional<M68kRegisterState>> in_states;
    std::deque<Address> worklist;
    std::set<Address> queued;
    // SEG-007-T188 / ADR-0029: one shared canonical control adjacency for both
    // root selection and the fixed point below.
    const auto adjacency = m68k_canonical_control_adjacency(entries, canonical_edges);
    const auto selected_roots = m68k_select_stitched_analysis_roots(
        entries, entry_roots, disconnected_root_candidates, canonical_edges);
    for (const auto &root : selected_roots) {
      if (root.space != TargetAddressSpace::m68k_program || !entries.contains(root.value)) continue;
      M68kRegisterState seed{};
      if (seeded_root_states) {
        if (const auto found_seed = seeded_root_states->find(root.value); found_seed != seeded_root_states->end())
          seed = found_seed->second;
      }
      in_states[root.value] = seed;
      if (queued.insert(root.value).second) worklist.push_back(root.value);
    }
    // SEG-007-T178 / ADR-0009 producer extension: per-analysis memo for the
    // bounded callee register-write footprint proof (same callee is hit
    // repeatedly along different reaching paths).
    std::map<Address, M68kRegisterFootprint> footprint_memo;
    while (!worklist.empty()) {
      const auto current = worklist.front();
      worklist.pop_front();
      queued.erase(current);
      const auto found = entries.find(current);
      if (found == entries.end()) continue;
      const auto out_state = m68k_apply_finite_value_transfer(*in_states[current], found->second, &environment);
      if (out_states_by_address) (*out_states_by_address)[current] = out_state;
      const auto adj = adjacency.find(current);
      const std::vector<M68kValueFlowSuccessor> empty_succ;
      for (const auto &successor : (adj != adjacency.end() ? adj->second : empty_succ)) {
        if (!entries.contains(successor.address)) continue;
        M68kRegisterState contributed;
        if (!successor.force_unknown) {
          contributed = out_state;
        } else if (!successor.callee_entry) {
          // Unknown callee (indirect JSR / JSR (mem) / non-foldable target):
          // the pre-T178 whole-state-unknown continuation stands.
          contributed = M68kRegisterState{};
        } else {
          // SEG-007-T178 / ADR-0009 producer extension: a finite An/Dn set
          // survives the call continuation iff the callee provably never
          // writes that register. A7 is never preserved (call push/pop).
          const auto footprint = m68k_compute_callee_register_footprint(
              *successor.callee_entry, entries, adjacency, max_call_frame_depth, footprint_memo);
          if (footprint.exhaustive_unknown) {
            contributed = M68kRegisterState{};
          } else {
            contributed = out_state;
            for (std::size_t i = 0; i < 8U; ++i) {
              if (footprint.dn_written[i]) contributed.dn[i] = M68kDnValueState{};
              // A7/SP is never finite-proof-eligible (SEG-007-T178 correction).
              if (!m68k_an_finite_proof_eligible(static_cast<std::uint8_t>(i)) || footprint.an_written[i])
                contributed.an[i] = M68kAnValueState{};
            }
          }
        }
        // SEG-007-T199 / ADR-0009 owner-1: refine the contributed state on this
        // specific edge from the preceding `CMP/CMPI #imm` + unsigned `Bcc`
        // truth before it is merged into the successor's IN state.
        if (successor.dn_constraint)
          m68k_apply_dn_edge_constraint(contributed, *successor.dn_constraint);
        auto &slot = in_states[successor.address];
        if (!slot) {
          slot = contributed;
          if (queued.insert(successor.address).second) worklist.push_back(successor.address);
          continue;
        }
        auto merged = m68k_merge_register_state(*slot, contributed);
        if (!m68k_register_state_equal(merged, *slot)) {
          slot = std::move(merged);
          if (queued.insert(successor.address).second) worklist.push_back(successor.address);
        }
      }
    }
    std::map<Address, M68kRegisterState> result;
    for (auto &[address, state] : in_states)
      if (state) result.emplace(address, *state);
    return result;
  }

  // SEG-007-T124 / ADR-0009: constructs the proven `M68kIndirectTargetEaSet`
  // for a brief PC-relative indexed control instruction, or returns false
  // when any proof condition fails (unknown/unbounded index value, empty or
  // non-24-bit/overflowing candidate). Never partial: on any failure `out`
  // is left in an unspecified state and the caller retains the existing
  // fail-closed `reached_unresolved_direct_edge`.
  [[nodiscard]] bool compute_indirect_target_set(Address addr, const M68kDecodedInstruction &decoded,
                                                  M68kIndirectTargetEaSet &out) const {
    const auto &ea = decoded.source_ea;
    // ADR-0009: An index and long index are initially unrepresentable.
    if (ea.index_is_address || ea.index_is_long) return false;
    const auto states = analyze_finite_index_values();
    const auto found = states.find(addr);
    if (found == states.end()) return false;
    const auto &index_state = found->second.dn[ea.index_reg];
    if (!index_state.finite || index_state.values.empty()) return false;
    std::vector<Address> raw_candidates;
    raw_candidates.reserve(index_state.values.size());
    for (const auto low_word : index_state.values) {
      const auto index_value = static_cast<std::int32_t>(static_cast<std::int16_t>(low_word));
      const auto raw = static_cast<std::int64_t>(ea.pc_base_address) +
                        static_cast<std::int64_t>(ea.displacement) + static_cast<std::int64_t>(index_value);
      if (raw < 0 || raw > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) return false;
      const auto candidate = static_cast<Address>(static_cast<std::uint32_t>(raw));
      if ((candidate & UINT32_C(0xFF000000)) != 0U) return false;  // overflow / non-24-bit EA evaluation
      raw_candidates.push_back(candidate);
    }
    std::sort(raw_candidates.begin(), raw_candidates.end());
    raw_candidates.erase(std::unique(raw_candidates.begin(), raw_candidates.end()), raw_candidates.end());
    if (raw_candidates.empty()) return false;
    out.source_instruction = decoded.provenance;
    out.control_ea = ea;
    out.index_values = M68kFiniteIndexValueSet{decoded.provenance, static_cast<DataRegister>(ea.index_reg),
                                                index_state.values};
    out.candidates.clear();
    out.candidates.reserve(raw_candidates.size());
    for (const auto value : raw_candidates) out.candidates.push_back({TargetAddressSpace::m68k_program, value});
    return true;
  }

 private:

  // SEG-021-T028: single place that records the existing Tier-2 fact once per
  // source; `indirect_emitted_` guarantees a source never holds both tiers.
  void record_unproven_tier2_owner(Address addr, const M68kDecodedInstruction &decoded, bool is_call) {
    if (!indirect_emitted_.insert(addr).second) return;
    M68kUnprovenIndirectControlEaSet unproven{};
    unproven.source_instruction = decoded.provenance;
    unproven.control_ea = decoded.source_ea;
    unproven.is_call = is_call;
    unproven_indirect_control_ea_sets_.push_back(unproven);
  }

  // SEG-007-T124 / ADR-0009, generalized per ADR-0011 Decision §§1-2: proves
  // and admits every candidate of a `M68kIndirectTargetEaSet`, then pushes
  // each admitted candidate onto `worklist_` (plus, for an indirect JSR, the
  // one shared continuation address every candidate returns to, per Decision
  // §1) instead of recursing into each immediately. Every candidate must
  // pass the existing direct-call target-admission rule (identical to the
  // foldable JMP/JSR path above) before any edge/frame/fact is retained at
  // all; only then are the source's typed fact, per-candidate edges (and,
  // for JSR, per-candidate call frames), and block entries recorded exactly
  // once. A candidate's own further exploration is classified by `run`'s
  // outer loop exactly like any other worklist item, preserving the same
  // T062/T064 best-effort sibling-exploration discipline this function
  // already used before this task.
  [[nodiscard]] bool process_indirect_control(Address addr, const M68kDecodedInstruction &decoded) {
    const bool is_call = decoded.kind == M68kInstructionKind::jsr;
    const auto &control_ea = decoded.source_ea;
    if (!control_ea.index_is_address && !control_ea.index_is_long) tier2_eligible_control_sources_.insert(addr);
    M68kIndirectTargetEaSet target_set{};
    if (!compute_indirect_target_set(addr, decoded, target_set)) {
      // SEG-007-T174 / ADR-0024: `process_indirect_control` is reached only
      // for the recognized `pc_index8` control-EA form (see the caller's own
      // mode check). This failure keeps discovery's own pre-existing
      // fail-closed `reached_unresolved_direct_edge` behavior COMPLETELY
      // UNCHANGED -- every existing ADR-0009/0022/0023 test's own "still
      // unresolved" assertion continues to hold exactly as before this ADR.
      // The only addition is recording the weaker Tier-2-eligible fact
      // (source provenance and decoded control EA only, no candidate list)
      // alongside the unchanged failure, but only for the word-size-Dn-index
      // shape both tiers' C11 lowering can actually compute (never An-indexed
      // or long-size, a genuinely different SHAPE neither tier represents).
      // Tier 2 is realized entirely downstream, at C4 emission time: the
      // generic `genesis_frontier_stop_<addr>` function C4 already builds for
      // this exact `reached_unresolved_direct_edge` frontier is replaced by a
      // Tier-2-aware one (same name, same `genesis_dispatch` arm, same
      // fail-closed default) whenever this fact is present -- see
      // libs/codegen/c11/src/frontend.cpp's `build_genesis_frontier_stop_function`.
      if (!control_ea.index_is_address && !control_ea.index_is_long &&
          indirect_emitted_.insert(addr).second) {
        M68kUnprovenIndirectControlEaSet unproven{};
        unproven.source_instruction = decoded.provenance;
        unproven.control_ea = control_ea;
        unproven.is_call = is_call;
        unproven_indirect_control_ea_sets_.push_back(unproven);
      }
      failure_ = provenance_issue(DirectFlowDiagnostic::reached_unresolved_direct_edge, decoded.provenance);
      return false;
    }
    for (const auto &candidate : target_set.candidates) {
      if (environment_.admit_target(candidate, M68kDiscoveryTargetRole::direct_call)) {
        // SEG-021-T028: the finite Tier-1 set cannot become authoritative
        // (a candidate fails admission), so it is discarded whole -- no
        // partial edges/frames/block entries -- and the source falls back to
        // the existing word-size-Dn Tier-2 fact exactly like a failed proof.
        // Tier-2 only compares the runtime-computed target against the
        // compiled-in emitted-code set, so an over-approximate finite set is
        // safe to abandon.
        if (!control_ea.index_is_address && !control_ea.index_is_long) {
          record_unproven_tier2_owner(addr, decoded, is_call);
        }
        failure_ = provenance_issue(DirectFlowDiagnostic::reached_unresolved_direct_edge, decoded.provenance);
        return false;
      }
    }
    if (indirect_emitted_.insert(addr).second) {
      indirect_target_ea_sets_.push_back(target_set);
      for (const auto &candidate : target_set.candidates) {
        note_block_entry(candidate, decoded.provenance);
        if (is_call) {
          const auto call = m68k_make_static_call(decoded, candidate.value);
          frames_.push_back(M68kStaticFrame{call});
          edges_.push_back({decoded.provenance, M68kStaticEdgeKind::indirect_call, candidate, call});
        } else {
          edges_.push_back({decoded.provenance, M68kStaticEdgeKind::indirect_branch, candidate, std::nullopt});
        }
      }
      if (is_call) {
        const auto next_pc_value = static_cast<Address>(addr + decoded.provenance.length.value);
        note_block_entry({TargetAddressSpace::m68k_program, next_pc_value}, decoded.provenance);
        enqueue_next_pc(next_pc_value, decoded.provenance);  // SEG-007-T181: stitch if a continuation boundary
      }
    }
    // SEG-007-T180: an ADR-0009 indirect candidate that is an independently
    // validated unit boundary is stitched (edge/frame/block already recorded
    // just above), not recursively walked here.
    for (const auto &candidate : target_set.candidates) enqueue_control_target(candidate.value);
    return true;
  }

  // SEG-007-T178 / ADR-0009 producer extension: constructs the proven
  // `M68kIndirectTargetEaSet` for a pure address-register-indirect control
  // instruction (`JMP (An)` / `JSR (An)`, EA mode 2, no displacement/index/
  // extension word). The proven An members ARE the canonical candidates
  // directly (EA = architectural `An`; no base/index/displacement
  // arithmetic). Returns false on any failed proof obligation; never partial.
  [[nodiscard]] bool compute_indirect_target_set_an(Address addr, const M68kDecodedInstruction &decoded,
                                                     M68kIndirectTargetEaSet &out) const {
    const auto &ea = decoded.source_ea;
    if (ea.mode != M68kEaMode::address_indirect || ea.displacement != 0 || ea.extension_words != 0U) return false;
    // SEG-007-T178 correction: A7/SP is excluded from the finite-An Tier-1
    // proof (see `m68k_an_finite_proof_eligible`); `JMP (A7)` / `JSR (A7)`
    // therefore stay fail-closed here regardless of any lattice state.
    if (!m68k_an_finite_proof_eligible(ea.reg)) return false;
    const auto states = analyze_finite_index_values();
    const auto found = states.find(addr);
    if (found == states.end()) return false;
    const auto &an_state = found->second.an[ea.reg];
    if (!an_state.finite || an_state.addrs.empty()) return false;
    std::vector<Address> raw_candidates;
    raw_candidates.reserve(an_state.addrs.size());
    for (const auto member : an_state.addrs) {
      if ((member & UINT32_C(0xFF000000)) != 0U) return false;  // non-24-bit EA
      raw_candidates.push_back(member);
    }
    std::sort(raw_candidates.begin(), raw_candidates.end());
    raw_candidates.erase(std::unique(raw_candidates.begin(), raw_candidates.end()), raw_candidates.end());
    if (raw_candidates.empty()) return false;
    out.source_instruction = decoded.provenance;
    out.control_ea = ea;
    // The An case has no Dn index; `same_ea` compares mode+reg so a default
    // `index_values` is fine.
    out.candidates.clear();
    out.candidates.reserve(raw_candidates.size());
    for (const auto value : raw_candidates) out.candidates.push_back({TargetAddressSpace::m68k_program, value});
    return true;
  }

  // SEG-007-T178 / ADR-0009 producer extension: the address-register-indirect
  // sibling of `process_indirect_control`. On failure it does NOT record a
  // `M68kUnprovenIndirectControlEaSet` (that Tier-2/ADR-0024 fact is the
  // `pc_index8` shape only) -- it retains the exact current fail-closed
  // `reached_unresolved_direct_edge`.
  [[nodiscard]] bool process_indirect_control_an(Address addr, const M68kDecodedInstruction &decoded) {
    const bool is_call = decoded.kind == M68kInstructionKind::jsr;
    // SEG-007-T178 correction: `JMP (A7)` / `JSR (A7)` are never Tier-1 provable
    // (A7/SP is excluded from the finite-An domain -- see
    // `m68k_an_finite_proof_eligible`). Fail closed immediately, without the
    // deferred-retry dance a genuinely An0-A6 site may need.
    if (!m68k_an_finite_proof_eligible(decoded.source_ea.reg)) {
      failure_ = provenance_issue(DirectFlowDiagnostic::reached_unresolved_direct_edge, decoded.provenance);
      return false;
    }
    tier2_eligible_control_sources_.insert(addr);
    M68kIndirectTargetEaSet target_set{};
    if (!compute_indirect_target_set_an(addr, decoded, target_set)) {
      // SEG-007-T178 / ADR-0009 producer extension: the finite An proof may
      // depend on a bounded callee register-write footprint (see
      // `m68k_compute_callee_register_footprint`), and a callee body reachable
      // only through this same walk may not be fully decoded yet at the moment
      // this site is first drained. Defer one retry until after the main
      // drain, when every reachable callee body is decoded (exactly the state
      // `synthesize_return_edges` already relies on). The retry runs with
      // `indirect_an_final_pass_` set, so a still-unprovable set then records
      // the unchanged fail-closed `reached_unresolved_direct_edge`.
      if (!indirect_an_final_pass_) {
        if (std::find(deferred_indirect_an_.begin(), deferred_indirect_an_.end(), addr) ==
            deferred_indirect_an_.end())
          deferred_indirect_an_.push_back(addr);
        return true;
      }
      // SEG-007-T179 / ADR-0025 Tier-2 generalization: the finite-An Tier-1
      // proof is genuinely unavailable for this pure `(An)` control site
      // (reg<7 -- A7/SP already failed closed above with NO recorded fact,
      // preserving the SEG-007-T178 exclusion). Additionally record the
      // weaker Tier-2-eligible fact, exactly the additive shape
      // `process_indirect_control` records for the `pc_index8` form: source
      // provenance and the decoded control EA only, no candidate list. The
      // primary fail-closed `reached_unresolved_direct_edge` below is
      // unchanged. Downstream C4 emission replaces the generic frontier stop
      // with a Tier-2-aware one that only ever compares `runtime->a[reg]`
      // against the compiled-in `EmittedCodeAddressSet` -- no target fetch or
      // decode. Tier-1 (`compute_indirect_target_set_an`) is untouched.
      if (indirect_emitted_.insert(addr).second) {
        M68kUnprovenIndirectControlEaSet unproven{};
        unproven.source_instruction = decoded.provenance;
        unproven.control_ea = decoded.source_ea;
        unproven.is_call = is_call;
        unproven_indirect_control_ea_sets_.push_back(unproven);
      }
      failure_ = provenance_issue(DirectFlowDiagnostic::reached_unresolved_direct_edge, decoded.provenance);
      return false;
    }
    for (const auto &candidate : target_set.candidates) {
      if (environment_.admit_target(candidate, M68kDiscoveryTargetRole::direct_call)) {
        record_unproven_tier2_owner(addr, decoded, is_call);
        failure_ = provenance_issue(DirectFlowDiagnostic::reached_unresolved_direct_edge, decoded.provenance);
        return false;
      }
    }
    if (indirect_emitted_.insert(addr).second) {
      indirect_target_ea_sets_.push_back(target_set);
      for (const auto &candidate : target_set.candidates) {
        note_block_entry(candidate, decoded.provenance);
        if (is_call) {
          const auto call = m68k_make_static_call(decoded, candidate.value);
          frames_.push_back(M68kStaticFrame{call});
          edges_.push_back({decoded.provenance, M68kStaticEdgeKind::indirect_call, candidate, call});
        } else {
          edges_.push_back({decoded.provenance, M68kStaticEdgeKind::indirect_branch, candidate, std::nullopt});
        }
      }
      if (is_call) {
        const auto next_pc_value = static_cast<Address>(addr + decoded.provenance.length.value);
        note_block_entry({TargetAddressSpace::m68k_program, next_pc_value}, decoded.provenance);
        enqueue_next_pc(next_pc_value, decoded.provenance);  // SEG-007-T181: stitch if a continuation boundary
      }
    }
    // SEG-007-T180: an ADR-0009 indirect candidate that is an independently
    // validated unit boundary is stitched (edge/frame/block already recorded
    // just above), not recursively walked here.
    for (const auto &candidate : target_set.candidates) enqueue_control_target(candidate.value);
    return true;
  }

  // SEG-021-T011 / ADR-0024/ADR-0025 Tier-2 generalization: the brief
  // address-register-indexed control EA sibling of `process_indirect_control`
  // (which owns the `pc_index8` shape) and `process_indirect_control_an`
  // (which owns the pure `(An)` shape) -- `JMP (d8,An,Xn)` / `JSR (d8,An,Xn)`.
  //
  // This shape's runtime target is `An + sign_extend(Xn) + d8`: BOTH the base
  // (a runtime An value) and the offset (a runtime Dn/An index value) are
  // register-dependent, unlike either existing Tier-1 producer (`pc_index8`
  // has a statically-known PC base; the pure `(An)` form has no index at
  // all). A Tier-1 finite-value proof for this combined shape would need a
  // genuinely new cross-product producer -- joining the existing finite-An-
  // state domain (ADR-0030, `analyze_finite_index_values()`'s `an[]` state)
  // with the existing finite-Dn/An-index-state domain (ADR-0009's own
  // `dn[]`/index producer) and proving every combination of the two remains
  // within the existing 256-member cap -- which this task's own scope
  // explicitly excludes as a new architecture decision ("do NOT build a new
  // cross-product finite-value producer"; see this task's Notes and the
  // dispatch site's own comment above). Every site of this shape therefore
  // skips any Tier-1 attempt entirely and goes straight to the existing
  // Tier-2/ADR-0024 generalization already used by `pc_index8`'s and the
  // pure `(An)` form's own failure paths: record the weaker Tier-2-eligible
  // fact (source provenance and decoded control EA only, no candidate list)
  // and keep the unchanged fail-closed `reached_unresolved_direct_edge`.
  // Downstream C4 emission (`build_genesis_frontier_stop_function` in
  // libs/codegen/c11/src/frontend.cpp) recognizes this `address_index8`
  // control-EA shape and replaces the generic frontier stop with a runtime
  // EA computation (`An + sign_extend(Xn) + d8`, the exact same formula the
  // shared `m68k_emit_runtime_ea_address` runtime-EA helper already computes
  // for LEA/PEA's own non-control `address_index8` admission) plus a binary-
  // search membership guard against the compiled-in `EmittedCodeAddressSet`
  // -- no target fetch or decode, exactly like the two existing Tier-2
  // shapes. `m68k_is_supported_computed_control_ea`
  // (libs/cpu/m68k/src/effective_address.cpp) is widened alongside this
  // producer so the existing multi-root aggregation supersession logic
  // (platforms/genesis/machine/src/frontend.cpp) recognizes this new shape
  // too.
  [[nodiscard]] bool process_indirect_control_index8(Address addr, const M68kDecodedInstruction &decoded) {
    const bool is_call = decoded.kind == M68kInstructionKind::jsr;
    if (indirect_emitted_.insert(addr).second) {
      M68kUnprovenIndirectControlEaSet unproven{};
      unproven.source_instruction = decoded.provenance;
      unproven.control_ea = decoded.source_ea;
      unproven.is_call = is_call;
      unproven_indirect_control_ea_sets_.push_back(unproven);
    }
    failure_ = provenance_issue(DirectFlowDiagnostic::reached_unresolved_direct_edge, decoded.provenance);
    return false;
  }

  // SEG-007-T178 / ADR-0009 producer extension: An-indirect control sites
  // whose finite-set proof was deferred for one post-drain retry, and the
  // flag that makes that retry authoritative (fail closed on a still-
  // unprovable set instead of deferring again).
  std::vector<Address> deferred_indirect_an_;
  bool indirect_an_final_pass_{false};

  M68kProgramAddress entry_{};
  M68kStaticDiscoveryLimits limits_{};
  M68kStaticDiscoveryEnvironment &environment_;
  // SEG-007-T180 / ADR-0026: offline inventory unit boundaries (see the free
  // function's own doc comment). Empty for every pre-T180 caller.
  const std::set<Address> &boundaries_;
  std::uint32_t stitched_boundary_edges_{};
  std::set<Address> stitched_boundary_addresses_;
  // SEG-007-T181 / ADR-0027: the SEPARATE fallthrough-continuation boundary set
  // (consulted only by `enqueue_next_pc`), the distinct-edge count, the
  // per-predecessor-address edge dedup guard, the set of `next_pc` addresses
  // actually enqueued (not stitched) so a ceiling trip can be classified, and
  // the ceiling-trip continuation frontier this pass exposes.
  const std::set<Address> &continuation_boundaries_;
  std::uint32_t stitched_fallthrough_continuation_edges_{};
  std::set<Address> continuation_edges_emitted_;
  std::set<Address> next_pc_reached_;
  std::vector<Address> fallthrough_continuation_frontier_;
  // SEG-007-T182 / ADR-0028: every statically-resolved+validated direct
  // control-transfer destination this pass enqueued for a recursive body walk
  // (taken direct-branch target, JSR/BSR/foldable JMP-JSR callee, ADR-0009
  // finite indirect candidate). A later ceiling trip whose address is a member
  // of this set but NOT a `next_pc` step is eligible for a Phase-2-synthesized
  // resolved-control-target unit (the A -> X control edge/frame was already
  // recorded by the caller; only X's body walk is partitioned). Empty for every
  // pre-T182 caller.
  std::set<Address> control_target_reached_;
  std::vector<Address> resolved_control_target_frontier_;

  M68kStaticDecodeCache decode_cache_;
  std::vector<Address> decode_order_;
  std::set<Address> block_entries_;
  std::vector<Address> block_entry_order_;
  std::uint32_t instructions_used_{};
  std::optional<M68kDiscoveryIssue> failure_;
  std::optional<M68kDiscoveryIssue> primary_issue_;
  std::optional<InstructionProvenance> completion_rts_;
  std::vector<M68kDiscoveryIssue> secondary_issues_;
  // ADR-0011 Decisions §§1-3: an explicit heap-allocated worklist over bare
  // addresses (Decision §2) and a bare-address visited set (Decision §1 drops
  // the `stack_signature` component entirely -- there is no more frame_stack
  // to key it with). ADR-0014 Decision §1 (M1) drains this worklist
  // first-in-first-out (breadth-first) rather than last-in-first-out; a
  // `std::deque` gives O(1) push-back/pop-front, matching ADR-0011 §2's
  // delegated container choice without reopening that decision (see `run`'s
  // own doc comment above).
  std::deque<Address> worklist_;
  std::set<Address> visited_states_;
  std::set<Address> branch_edges_emitted_;
  std::set<Address> calls_emitted_;
  std::vector<std::pair<Address, InstructionProvenance>> accepted_branch_targets_;
  std::vector<M68kStaticEdge> edges_;
  std::vector<M68kStaticFrame> frames_;
  // SEG-007-T124 / ADR-0009: guards one-time retention of a computed control
  // instruction's own typed fact/edges/frames (mirrors `calls_emitted_`'s
  // per-source-address idempotency for the direct-call case), keyed by the
  // source instruction's own address.
  std::set<Address> indirect_emitted_;
  std::vector<M68kIndirectTargetEaSet> indirect_target_ea_sets_;
  // SEG-007-T174 / ADR-0024: one entry per source instruction whose
  // recognized `pc_index8` control EA failed the finite-value-set proof (see
  // `process_indirect_control`'s failure branch). One address is visited at
  // most once by `run`'s own `visited_states_` guard, so no separate
  // idempotency set is needed here.
  std::vector<M68kUnprovenIndirectControlEaSet> unproven_indirect_control_ea_sets_;
  // SEG-021-T028: retained computed-control sources eligible for a Tier-2 owner.
  std::set<Address> tier2_eligible_control_sources_;
  // Set by `resolve_operand` immediately before a diagnostic-carrying return;
  // consumed (and reset) by `reject_operand` in the same decode_instruction
  // invocation. Reset defensively at the start of every decode_instruction
  // call as well, though the pre-T003 monolith's own equivalent
  // `frontier_access` capture never actually required it (every resolve call
  // that sets it is immediately followed by a `reject_selected`-equivalent
  // return in the same switch case).
  std::optional<M68kCpuMemoryAccessRequest> pending_access_;
};

}  // namespace

M68kStaticDiscoveryResult discover_m68k_static_graph(M68kProgramAddress entry,
                                                       const M68kStaticDiscoveryLimits &limits,
                                                       M68kStaticDiscoveryEnvironment &environment,
                                                       const std::set<std::uint32_t> &independent_unit_boundaries,
                                                       const std::set<std::uint32_t> &fallthrough_continuation_boundaries) {
  M68kStaticGraphWalker walker(entry, limits, environment, independent_unit_boundaries,
                               fallthrough_continuation_boundaries);
  return walker.run();
}

// SEG-007-T189 / ADR-0030: the maximum number of distinct direct-call-site
// contexts separately re-walked and unioned at one shared computed-control
// site. A fixed, small, documented bound -- analogous in spirit to the
// existing 256-member finite-set cap, but bounding analysis WORK (one bounded
// re-walk of the already-terminating context-insensitive fixed point per
// tracked context) rather than a single register's value-set size. Beyond
// this many distinguishable finite-contributing call sites reaching one
// shared site, the excess call sites are not separately tracked; their
// contribution is exactly what the ordinary context-insensitive base walk
// already computes for them (conservative widening, never a fabricated
// fact). See ADR-0030 "Widening at the bound".
inline constexpr std::size_t kM68kMaxTrackedCallContexts = 4U;

// SEG-007-T189 / ADR-0030 (corrected selection policy): bounded forward
// reachability search over the SAME canonical control adjacency
// (`m68k_canonical_control_adjacency`) already used everywhere else in this
// file -- never a second graph. Determines whether `target` is reachable
// from `start`. Terminates unconditionally: the visited-address set is
// bounded by the finite decoded-entry count, exactly the same discipline
// `M68kStaticGraphWalker::analyze_finite_register_values`'s own worklist and
// `m68k_compute_callee_register_footprint`'s own visited set already rely on
// for termination over this graph. Used ONLY to decide which finite-
// contributing call-site context is relevant to a specific still-unresolved
// computed-control site -- not a new reachability authority.
[[nodiscard]] bool m68k_context_reaches(
    Address start, Address target, const std::map<Address, std::vector<M68kValueFlowSuccessor>> &adjacency) {
  if (start == target) return true;
  std::set<Address> visited{start};
  std::deque<Address> work{start};
  while (!work.empty()) {
    const auto current = work.front();
    work.pop_front();
    const auto adj = adjacency.find(current);
    if (adj == adjacency.end()) continue;
    for (const auto &succ : adj->second) {
      if (succ.address == target) return true;
      if (visited.insert(succ.address).second) work.push_back(succ.address);
    }
  }
  return false;
}

// SEG-007-T189 / ADR-0030 (round 3, dual seed-point correction): bounded
// call-context-sensitive extension of the ADR-0009 finite-An proof. Context
// identity is still exactly one direct-call source-instruction address
// (depth 1, no call-string stacking -- see the ADR); the context-INSENSITIVE
// base walk below is always computed first and is itself one admissible
// context ("root"). A direct-call site is a CANDIDATE context when its own
// contributed register state (the base walk's post-instruction state at
// that call site -- unaffected by this task, computed identically to every
// prior release) is finite for at least one eligible An.
//
// A candidate is separately re-walked as a TRACKED (call site, seed point)
// UNIT only when it is both REGISTER-RELEVANT and REACHABILITY-RELEVANT to
// a specific still-unresolved computed-control site: register-relevant
// means the candidate's own contributed state is finite for the EXACT An
// register that unresolved site's `JMP`/`JSR (An)` indexes (a candidate
// finite only for A0 never consumes a tracking slot for an unresolved
// `JMP (A3)`); reachability-relevant means the candidate can reach that
// site, over the same canonical adjacency, from ONE of two seed points:
//
//   - its own CONTINUATION address (the ordinary caller-side post-return
//     reconvergence sub-shape: the finite fact is established in the
//     caller, survives the direct call via the existing bounded
//     callee-write-footprint proof, and the shared site is reached only
//     AFTER the call returns); or
//   - the call's own CALLEE ENTRY address (the direct-call-into-ambiguous-
//     callee-entry sub-shape: the shared computed-control site is reached
//     deterministically from inside the body of the very callee this call
//     site directly calls, never via this call's own post-return
//     continuation).
//
// A call site can be relevant via BOTH seed points at once (for the same or
// different unresolved sites) -- each relevant (call site, seed point) pair
// is its own tracked unit, re-walked independently and unioned at
// consumption exactly like any other admissible context. Sites the base
// walk already resolves need no additional context and never influence
// selection at all. This targets the small fixed tracking budget at units
// that can actually help a specific unresolved site's specific register,
// instead of the smallest-global-address top-K candidates regardless of
// relevance.
//
// For every tracked (call site, seed point) unit, this proof re-runs the
// SAME unmodified `analyze_finite_register_values` fixed point, seeded at
// that one seed address with that one contributed state, over the complete
// `decoded_by_address` / `canonical_edges` post-stitch graph (never a second
// graph). The contributed state differs by seed point: a CONTINUATION seed
// reuses the existing bounded callee-write-footprint proof (the callee has
// already run by the time control reaches the continuation, so only the
// registers it provably never writes survive); a CALLEE-ENTRY seed uses the
// RAW call-site contributed state directly, with NO footprint filtering --
// the callee has not executed yet at its own entry, so it sees the caller's
// pre-call/call-site register state unmodified. Applying the footprint
// transform at a callee-entry seed would answer the wrong question (it
// proves survival INTO the caller's post-return continuation, not the
// state visible AT the callee's own entry).
//
// At each computed `JMP (An)` site the final candidate set is the union of
// every admissible context's OWN independently finite An value set (base
// walk included); a context that is unknown there contributes nothing and
// is never promoted, so a genuine finite-plus-unknown join at a site with no
// tracked unit still yields exactly `unknown`, unchanged. The existing
// per-register 256-member cap and the A7/SP exclusion apply identically to
// the union as they always did to a single state.
enum class M68kCallContextSeedKind : std::uint8_t { continuation, callee_entry };
std::vector<M68kIndirectTargetEaSet> m68k_prove_stitched_an_indirect_targets(
    const std::map<std::uint32_t, M68kDecodedInstruction> &decoded_by_address,
    const std::vector<M68kProgramAddress> &entry_roots,
    const std::vector<M68kStaticEdge> &canonical_edges, std::uint32_t max_call_frame_depth,
    M68kStaticDiscoveryEnvironment &environment,
    const std::vector<M68kProgramAddress> &disconnected_root_candidates) {
  std::map<Address, M68kRegisterState> out_states_by_address;
  const auto states = M68kStaticGraphWalker::analyze_finite_register_values(
      decoded_by_address, entry_roots, canonical_edges, max_call_frame_depth, environment,
      disconnected_root_candidates, /*seeded_root_states=*/nullptr, &out_states_by_address);

  // Identify every direct-call site whose OWN contributed state (computed by
  // the base walk above, never recomputed differently) is already finite for
  // at least one eligible An -- a candidate for a separately tracked context.
  // Sorted by call-site address for a deterministic, order-independent (of
  // worklist processing order) selection: the selection depends only on the
  // SET of interesting call sites the (already order-independent, per
  // ADR-0029 Decision 9 / T188) base walk produced, never on any traversal
  // order. NOTE: this set is NOT yet truncated to the tracked-context cap
  // here -- see the reachability-directed selection immediately below, which
  // decides which of these are actually WORTH tracking before any cap is
  // applied.
  struct InterestingContext {
    Address call_site{};
    Address callee_entry{};
    Address continuation{};
  };
  std::vector<InterestingContext> interesting;
  for (const auto &edge : canonical_edges) {
    if (edge.kind != M68kStaticEdgeKind::direct_call || !edge.call) continue;
    const auto call_site = edge.source_instruction.source.address.value;
    const auto found = out_states_by_address.find(call_site);
    if (found == out_states_by_address.end()) continue;
    bool any_finite = false;
    for (std::uint8_t reg = 0; reg < 8U && !any_finite; ++reg)
      if (m68k_an_finite_proof_eligible(reg) && found->second.an[reg].finite) any_finite = true;
    if (any_finite)
      interesting.push_back({call_site, edge.call->callee.value, edge.call->continuation.value});
  }
  std::sort(interesting.begin(), interesting.end(),
            [](const InterestingContext &a, const InterestingContext &b) { return a.call_site < b.call_site; });

  // SEG-007-T189 / ADR-0030 (round 3, dual seed-point / register-aware
  // selection): a finite-contributing call site is only worth separately
  // re-walking, at a specific seed point, if it is BOTH register-relevant
  // (its own contributed state is finite for the EXACT An register a
  // specific still-unresolved computed-control site indexes -- a candidate
  // finite only for A0 must never consume a tracking slot for an unresolved
  // `JMP (A3)`) AND reachability-relevant from that seed point (it can
  // reach that exact site, over the same canonical adjacency, from either
  // its own CONTINUATION address or its own CALLEE-ENTRY address -- see the
  // enum/doc comment above). A site the base walk already resolves needs no
  // additional context at all. First collect every still-unresolved
  // eligible computed-control site (JMP/JSR (An) where the BASE walk's own
  // state is not finite for the indexed register); then, for each such
  // site and each candidate call site whose base contributed state is
  // finite for THAT site's exact register, run the bounded forward
  // reachability search (`m68k_context_reaches`, over the SAME canonical
  // adjacency used everywhere else) from both the candidate's continuation
  // and its callee entry, recording each reachable (call site, seed point)
  // pair as its own relevant TRACKED UNIT -- a single call site may
  // therefore be relevant via one seed point for one unresolved site and
  // the other seed point for a different unresolved site; both are tracked
  // independently when budget allows. The deterministic
  // smallest-call-site-address, continuation-before-callee-entry tie-break
  // selects up to `kM68kMaxTrackedCallContexts` of the relevant UNITS (not
  // call sites) for the one bounded re-walk each unit gets (never once per
  // (site, unit) pair -- the SET of relevant units is computed first, then
  // each is re-walked exactly once).
  std::vector<std::pair<Address, std::uint8_t>> unresolved_sites;
  for (const auto &[address, decoded] : decoded_by_address) {
    if ((decoded.kind != M68kInstructionKind::jmp && decoded.kind != M68kInstructionKind::jsr) ||
        decoded.source_ea.mode != M68kEaMode::address_indirect || decoded.source_ea.displacement != 0 ||
        decoded.source_ea.extension_words != 0U || !m68k_an_finite_proof_eligible(decoded.source_ea.reg))
      continue;
    const auto reg = decoded.source_ea.reg;
    const auto state = states.find(address);
    const bool base_finite = state != states.end() && state->second.an[reg].finite;
    if (!base_finite) unresolved_sites.push_back({address, reg});
  }
  // Reused below (unchanged from the pre-existing code) to seed each
  // selected unit's own re-walk and to compute its callee-write footprint
  // (continuation seed only) -- computed once here, ahead of that use, so
  // both the reachability search and the re-walk seeding share the
  // identical adjacency instance.
  const auto context_adjacency = m68k_canonical_control_adjacency(decoded_by_address, canonical_edges);

  struct TrackedUnit {
    Address call_site{};
    Address callee_entry{};
    Address continuation{};
    M68kCallContextSeedKind seed_kind{};
  };
  // (call_site, seed_kind) -> relevant, deduplicated so the same unit is
  // never counted twice even if it helps multiple unresolved sites.
  std::set<std::pair<Address, std::uint8_t>> relevant_keys;
  for (const auto &[unresolved_address, reg] : unresolved_sites) {
    for (const auto &context : interesting) {
      // Register-aware relevance: this candidate must itself be finite for
      // the EXACT register the unresolved site indexes.
      const auto found = out_states_by_address.find(context.call_site);
      if (found == out_states_by_address.end() || !found->second.an[reg].finite) continue;
      const std::pair<Address, std::uint8_t> continuation_key{
          context.call_site, static_cast<std::uint8_t>(M68kCallContextSeedKind::continuation)};
      if (!relevant_keys.contains(continuation_key) &&
          m68k_context_reaches(context.continuation, unresolved_address, context_adjacency))
        relevant_keys.insert(continuation_key);
      const std::pair<Address, std::uint8_t> callee_entry_key{
          context.call_site, static_cast<std::uint8_t>(M68kCallContextSeedKind::callee_entry)};
      if (!relevant_keys.contains(callee_entry_key) &&
          m68k_context_reaches(context.callee_entry, unresolved_address, context_adjacency))
        relevant_keys.insert(callee_entry_key);
    }
  }
  std::vector<TrackedUnit> relevant_units;
  relevant_units.reserve(relevant_keys.size());
  // `interesting` is already sorted by call-site address; within a call
  // site, the continuation-seeded unit is always considered before the
  // callee-entry-seeded unit, giving a fully deterministic total order.
  for (const auto &context : interesting) {
    const std::pair<Address, std::uint8_t> continuation_key{
        context.call_site, static_cast<std::uint8_t>(M68kCallContextSeedKind::continuation)};
    if (relevant_keys.contains(continuation_key))
      relevant_units.push_back(
          {context.call_site, context.callee_entry, context.continuation, M68kCallContextSeedKind::continuation});
    const std::pair<Address, std::uint8_t> callee_entry_key{
        context.call_site, static_cast<std::uint8_t>(M68kCallContextSeedKind::callee_entry)};
    if (relevant_keys.contains(callee_entry_key))
      relevant_units.push_back(
          {context.call_site, context.callee_entry, context.continuation, M68kCallContextSeedKind::callee_entry});
  }
  if (relevant_units.size() > kM68kMaxTrackedCallContexts) relevant_units.resize(kM68kMaxTrackedCallContexts);

  // For a CONTINUATION-seeded unit, the re-walk is rooted at the call's own
  // continuation address (back in the calling routine), reusing the SAME
  // already-validated bounded callee-write-footprint proof
  // (`m68k_compute_callee_register_footprint`) the base walk's own merge
  // logic uses to compute the identical footprint-adjusted contributed
  // state -- the callee has already executed and returned by the time
  // control reaches this seed point, so only registers it provably never
  // writes survive. `m68k_canonical_control_adjacency` deliberately omits
  // any RTS-to-continuation edge (structural return topology is owned
  // elsewhere), so a re-walk rooted at `callee_entry` could never reach
  // code that only becomes live again once the callee returns -- exactly
  // why the continuation-seeded unit is seeded at `continuation`, not
  // `callee_entry`.
  //
  // For a CALLEE-ENTRY-seeded unit, the re-walk is rooted at the callee's
  // own entry, with the RAW call-site contributed state (no footprint
  // filtering): the callee has not executed yet at its own entry, so it
  // sees the caller's pre-call/call-site register state directly. Applying
  // the footprint transform here would incorrectly filter registers the
  // callee itself writes LATER in its body, before this proof has any
  // chance to observe their pre-write, still-finite value at entry.
  std::map<Address, M68kRegisterFootprint> t189_footprint_memo;
  std::vector<std::map<Address, M68kRegisterState>> context_states;
  context_states.reserve(relevant_units.size());
  for (const auto &unit : relevant_units) {
    Address seed_address{};
    M68kRegisterState contributed{};
    if (unit.seed_kind == M68kCallContextSeedKind::continuation) {
      seed_address = unit.continuation;
      const auto footprint = m68k_compute_callee_register_footprint(
          unit.callee_entry, decoded_by_address, context_adjacency, max_call_frame_depth, t189_footprint_memo);
      if (!footprint.exhaustive_unknown) {
        contributed = out_states_by_address.at(unit.call_site);
        for (std::size_t i = 0; i < 8U; ++i) {
          if (footprint.dn_written[i]) contributed.dn[i] = M68kDnValueState{};
          if (!m68k_an_finite_proof_eligible(static_cast<std::uint8_t>(i)) || footprint.an_written[i])
            contributed.an[i] = M68kAnValueState{};
        }
      }
    } else {
      seed_address = unit.callee_entry;
      contributed = out_states_by_address.at(unit.call_site);
    }
    const std::map<Address, M68kRegisterState> seed{{seed_address, contributed}};
    context_states.push_back(M68kStaticGraphWalker::analyze_finite_register_values(
        decoded_by_address, {{TargetAddressSpace::m68k_program, seed_address}}, canonical_edges,
        max_call_frame_depth, environment, /*disconnected_root_candidates=*/{}, &seed));
  }

  std::vector<M68kIndirectTargetEaSet> results;
  for (const auto &[address, decoded] : decoded_by_address) {
    if ((decoded.kind != M68kInstructionKind::jmp && decoded.kind != M68kInstructionKind::jsr) ||
        decoded.source_ea.mode != M68kEaMode::address_indirect || decoded.source_ea.displacement != 0 ||
        decoded.source_ea.extension_words != 0U || !m68k_an_finite_proof_eligible(decoded.source_ea.reg))
      continue;
    const auto reg = decoded.source_ea.reg;
    std::vector<Address> union_candidates;
    bool any_finite = false;
    const auto accumulate = [&](const M68kAnValueState &an) {
      if (!an.finite || an.addrs.empty()) return;
      any_finite = true;
      union_candidates.insert(union_candidates.end(), an.addrs.begin(), an.addrs.end());
    };
    if (const auto state = states.find(address); state != states.end()) accumulate(state->second.an[reg]);
    for (const auto &context_state : context_states)
      if (const auto found = context_state.find(address); found != context_state.end())
        accumulate(found->second.an[reg]);
    if (!any_finite) continue;
    std::sort(union_candidates.begin(), union_candidates.end());
    union_candidates.erase(std::unique(union_candidates.begin(), union_candidates.end()), union_candidates.end());
    // Same 256-member cap the single-context path always enforced, now
    // applied to the union: an admissible context set that would push the
    // union past the cap is never partially retained.
    if (union_candidates.empty() || union_candidates.size() > 256U) continue;
    bool invalid = false;
    for (const auto member : union_candidates)
      if ((member & UINT32_C(0xFF000000)) != 0U) { invalid = true; break; }
    if (invalid) continue;
    M68kIndirectTargetEaSet set{};
    set.source_instruction = decoded.provenance;
    set.control_ea = decoded.source_ea;
    for (const auto candidate : union_candidates)
      set.candidates.push_back({TargetAddressSpace::m68k_program, candidate});
    results.push_back(std::move(set));
  }

  // SEG-007-T195 / ADR-0009: the same post-stitch re-proof, extended to the
  // brief PC-relative indexed control shape (`JMP (d8,PC,Dn.W)`), which the
  // intra-unit `compute_indirect_target_set` already proves but only over one
  // offline-inventory partition unit's local decode cache. When the finite
  // index value-flow crosses a `bsr` whose callee lives in a different
  // partition unit, the per-unit bounded callee-register-write footprint proof
  // cannot see the callee's decode and conservatively wipes the index
  // register; over `decoded_by_address` (the complete post-stitch aggregate)
  // the identical fixed point keeps the index finite.
  //
  // Unlike the `(An)` post-stitch proof above, this loop does NOT consume
  // `context_states`. Those contexts are selected exclusively by ADR-0030's
  // An-specific bounded call-context mechanism (interesting call sites chosen
  // because some eligible An is finite; only unresolved `JMP/JSR (An)` sites
  // considered; relevance tested against the exact An register; the tracked
  // budget allocated from unresolved An sites). The `(An)` post-stitch proof
  // therefore retains ADR-0030's context-sensitive union; the `pc_index8`
  // post-stitch proof uses the complete merged BASE fixed point only, which is
  // sufficient for the diagnosed cross-partition callee-footprint failure (an
  // intra-unit-vs-merged-graph problem, not a context-sensitivity one). Dn
  // context sensitivity remains deliberately unimplemented -- generalising
  // ADR-0030 to Dn is out of scope here. Same 256-member cap and same
  // fail-closed reject on any non-24-bit / overflowing candidate as the An
  // branch.
  for (const auto &[address, decoded] : decoded_by_address) {
    if (decoded.kind != M68kInstructionKind::jmp && decoded.kind != M68kInstructionKind::jsr) continue;
    const auto &ea = decoded.source_ea;
    if (ea.mode != M68kEaMode::pc_index8 || ea.index_is_address || ea.index_is_long) continue;
    const auto reg = ea.index_reg;
    std::vector<std::uint16_t> union_index_values;
    bool any_finite = false;
    const auto accumulate_dn = [&](const M68kDnValueState &dn) {
      if (!dn.finite || dn.values.empty()) return;
      any_finite = true;
      union_index_values.insert(union_index_values.end(), dn.values.begin(), dn.values.end());
    };
    if (const auto state = states.find(address); state != states.end()) accumulate_dn(state->second.dn[reg]);
    if (!any_finite) continue;
    std::sort(union_index_values.begin(), union_index_values.end());
    union_index_values.erase(std::unique(union_index_values.begin(), union_index_values.end()),
                             union_index_values.end());
    if (union_index_values.empty() || union_index_values.size() > 256U) continue;
    std::vector<Address> union_candidates;
    bool invalid = false;
    for (const auto low_word : union_index_values) {
      const auto index_value = static_cast<std::int32_t>(static_cast<std::int16_t>(low_word));
      const auto raw = static_cast<std::int64_t>(ea.pc_base_address) +
                       static_cast<std::int64_t>(ea.displacement) + static_cast<std::int64_t>(index_value);
      if (raw < 0 || raw > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
        invalid = true;
        break;
      }
      const auto candidate = static_cast<Address>(static_cast<std::uint32_t>(raw));
      if ((candidate & UINT32_C(0xFF000000)) != 0U) {
        invalid = true;
        break;
      }
      union_candidates.push_back(candidate);
    }
    if (invalid) continue;
    std::sort(union_candidates.begin(), union_candidates.end());
    union_candidates.erase(std::unique(union_candidates.begin(), union_candidates.end()), union_candidates.end());
    if (union_candidates.empty()) continue;
    M68kIndirectTargetEaSet set{};
    set.source_instruction = decoded.provenance;
    set.control_ea = ea;
    set.index_values = M68kFiniteIndexValueSet{decoded.provenance, static_cast<DataRegister>(reg),
                                               union_index_values};
    for (const auto candidate : union_candidates)
      set.candidates.push_back({TargetAddressSpace::m68k_program, candidate});
    results.push_back(std::move(set));
  }
  return results;
}

}  // namespace segarecomp
