#include "segarecomp/cpu/m68k/decode.hpp"

namespace segarecomp {

M68kCpuFrontierKind classify_m68k_cpu_frontier(const InstructionProvenance &provenance) {
  const auto word = static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(provenance.bytes[0]) << 8U) | provenance.bytes[1]);
  if (word == UINT16_C(0x4E71) && provenance.length.value == 2U)
    return M68kCpuFrontierKind::nop;
  if (word == UINT16_C(0x4E72) && provenance.length.value == 4U)
    return M68kCpuFrontierKind::stop_immediate_word;
  // RESET (opcode word 0100 1110 0111 0000 = 0x4E70), a fixed, no-operand,
  // no-extension-word System Control Group instruction, per the public
  // Motorola MC68000 Programmer's Reference Manual / M68000 Family
  // Instruction Set Summary (System Control Group opcode-word row), the same
  // citation standard SEG-007-T008/T023/T024/T025 already use.
  if (word == UINT16_C(0x4E70) && provenance.length.value == 2U)
    return M68kCpuFrontierKind::reset;
  // MOVE An,USP (opcode word 0100 1110 0110 0AAA = 0x4E60 | An, An in bits
  // 2..0), a fixed, no-extension-word System Control Group form, per the
  // public Motorola MC68000 Programmer's Reference Manual / M68000 Family
  // Instruction Set Summary (System Control Group opcode-word row), the same
  // citation standard the existing RESET/NOP/STOP recognizers above already
  // use. Only the eight legal `An` source encodings (0x4E60-0x4E67) match;
  // the reverse direction MOVE USP,An (0x4E68-0x4E6F) is a distinct bit
  // pattern (bit 3 set) and is deliberately excluded here.
  if (word >= UINT16_C(0x4E60) && word <= UINT16_C(0x4E67) && provenance.length.value == 2U)
    return M68kCpuFrontierKind::move_an_to_usp;
  return M68kCpuFrontierKind::none;
}

const char *m68k_instruction_kind_name(M68kInstructionKind kind) noexcept {
  switch (kind) {
  case M68kInstructionKind::moveq: return "moveq";
  case M68kInstructionKind::subq_l_1_d0: return "subq_l_1_d0";
  case M68kInstructionKind::bne_short: return "bne_short";
  case M68kInstructionKind::bra_short: return "bra_short";
  case M68kInstructionKind::rts: return "rts";
  case M68kInstructionKind::rte: return "rte";
  case M68kInstructionKind::tst: return "tst";
  case M68kInstructionKind::cmp: return "cmp";
  case M68kInstructionKind::cmpi: return "cmpi";
  case M68kInstructionKind::cmpa: return "cmpa";
  case M68kInstructionKind::add: return "add";
  case M68kInstructionKind::adda: return "adda";
  case M68kInstructionKind::addi: return "addi";
  case M68kInstructionKind::addq: return "addq";
  case M68kInstructionKind::sub: return "sub";
  case M68kInstructionKind::suba: return "suba";
  case M68kInstructionKind::subi: return "subi";
  case M68kInstructionKind::subq: return "subq";
  case M68kInstructionKind::logical_and: return "and";
  case M68kInstructionKind::andi: return "andi";
  case M68kInstructionKind::logical_or: return "or";
  case M68kInstructionKind::ori: return "ori";
  case M68kInstructionKind::eor: return "eor";
  case M68kInstructionKind::eori: return "eori";
  case M68kInstructionKind::move: return "move";
  case M68kInstructionKind::movea: return "movea";
  case M68kInstructionKind::clr: return "clr";
  case M68kInstructionKind::not_operand: return "not";
  case M68kInstructionKind::negate_word: return "neg_w";
  case M68kInstructionKind::lea: return "lea";
  case M68kInstructionKind::jmp: return "jmp";
  case M68kInstructionKind::jsr: return "jsr";
  case M68kInstructionKind::swap: return "swap";
  case M68kInstructionKind::ext_w: return "ext_w";
  case M68kInstructionKind::ext_l: return "ext_l";
  case M68kInstructionKind::pea: return "pea";
  case M68kInstructionKind::link: return "link";
  case M68kInstructionKind::unlk: return "unlk";
  case M68kInstructionKind::btst: return "btst";
  case M68kInstructionKind::bchg: return "bchg";
  case M68kInstructionKind::bclr: return "bclr";
  case M68kInstructionKind::bset: return "bset";
  case M68kInstructionKind::branch: return "branch";
  case M68kInstructionKind::bsr: return "bsr";
  case M68kInstructionKind::dbcc: return "dbcc";
  case M68kInstructionKind::movem: return "movem";
  case M68kInstructionKind::shift_rotate: return "shift_rotate";
  case M68kInstructionKind::move_an_to_usp: return "move_an_to_usp";
  case M68kInstructionKind::move_to_sr: return "move_to_sr";
  case M68kInstructionKind::nop: return "nop";
  case M68kInstructionKind::move_from_sr: return "move_from_sr";
  case M68kInstructionKind::move_to_ccr: return "move_to_ccr";
  case M68kInstructionKind::multiply_signed_word: return "muls";
  case M68kInstructionKind::multiply_unsigned_word: return "mulu";
  case M68kInstructionKind::divide_signed_word: return "divs";
  case M68kInstructionKind::divide_unsigned_word: return "divu";
  }
  return "unknown";
}

const char *m68k_probe_instruction_kind_name(const M68kDecodedInstruction &instruction) noexcept {
  if (instruction.kind == M68kInstructionKind::jsr && instruction.source_ea.mode == M68kEaMode::absolute_long)
    return "jsr_absolute_long";
  if (instruction.kind == M68kInstructionKind::move && instruction.size == M68kMemoryAccessWidth::long_word &&
      instruction.source_ea.mode == M68kEaMode::data_register && instruction.source_ea.reg == 0U &&
      instruction.destination_ea.mode == M68kEaMode::absolute_long)
    return "move_l_d0_absolute_long";
  if (instruction.kind == M68kInstructionKind::move && instruction.size == M68kMemoryAccessWidth::long_word &&
      instruction.source_ea.mode == M68kEaMode::absolute_long &&
      instruction.destination_ea.mode == M68kEaMode::data_register && instruction.destination_ea.reg == 1U)
    return "move_l_absolute_long_d1";
  if (instruction.kind == M68kInstructionKind::tst && instruction.size == M68kMemoryAccessWidth::long_word &&
      instruction.source_ea.mode == M68kEaMode::absolute_long)
    return "tst_l_absolute_long";
  return m68k_instruction_kind_name(instruction.kind);
}

namespace {
[[nodiscard]] RejectedM68kDecode reject(DecodeOutcome outcome, DecodeSource source,
                                         std::uint64_t available, std::uint32_t requested,
                                         bool has_length) {
  return {outcome, source, available, requested, has_length ? 2U : 0U, has_length, {}, false, false};
}

// ---------------------------------------------------------------------------
// SEG-007-T023: the one shared general effective-address decode layer used by
// every whitelisted TST/MOVE/MOVEA/CLR/LEA/JMP/JSR form. See
// docs/references/m68k-common-startup-data-movement-batch-contract.md,
// "The general 6-bit effective-address field, extension-word counts, and
// word order".

[[nodiscard]] M68kEaLegalMask m68k_ea_bit_for(M68kEaMode mode) noexcept {
  switch (mode) {
  case M68kEaMode::data_register: return m68k_ea_dn;
  case M68kEaMode::address_register: return m68k_ea_an;
  case M68kEaMode::address_indirect: return m68k_ea_an_indirect;
  case M68kEaMode::address_postinc: return m68k_ea_an_postinc;
  case M68kEaMode::address_predec: return m68k_ea_an_predec;
  case M68kEaMode::address_disp16: return m68k_ea_an_disp16;
  case M68kEaMode::absolute_word: return m68k_ea_absolute_word;
  case M68kEaMode::absolute_long: return m68k_ea_absolute_long;
  case M68kEaMode::pc_disp16: return m68k_ea_pc_disp16;
  case M68kEaMode::immediate: return m68k_ea_immediate;
  case M68kEaMode::address_index8: return m68k_ea_index8;
  case M68kEaMode::pc_index8: return m68k_ea_pc_index8;
  case M68kEaMode::unused: return 0U;
  }
  return 0U;
}

enum class M68kEaParseStatus { ok, illegal_mode, not_permitted, truncated };
struct M68kEaParseResult {
  M68kEaParseStatus status{M68kEaParseStatus::illegal_mode};
  M68kEffectiveAddress ea{};
  std::uint32_t extension_bytes{};
};

// Decodes one general 6-bit EA field (`mode3`/`reg3`, already split from the
// instruction word) against `legal`. Reads no byte beyond `available` at
// `ext_offset`; a truncated operand reports the exact byte count it needed
// (`extension_bytes`) and reads nothing. `ext_word_address` is the M68K
// program address of the first extension word this operand would consume
// (used by pc_disp16/pc_index8, whose base is "the address of the extension
// word" per the contract, not the post-instruction PC). SEG-007-T120:
// mode3==6 is brief-format (d8,An,Xn) indexed addressing, decoded here and
// gated by the instruction's legal mask like every other mode; a nonzero
// scale or full-format extension word is rejected as `illegal_mode`.
// SEG-007-T124 / ADR-0009: mode3==7, reg3==3 is the PC-relative indexed form
// (d8,PC,Xn), decoded identically (same brief-extension-word shape, PC base
// instead of An) and gated by the caller's own legal mask -- only
// `m68k_ea_jsr_jmp_control_modes` includes it, so every other selected form
// still rejects it exactly as before.
[[nodiscard]] M68kEaParseResult m68k_parse_ea_field(std::uint8_t mode3, std::uint8_t reg3, M68kEaLegalMask legal,
                                                     M68kMemoryAccessWidth size, std::span<const std::uint8_t> image,
                                                     std::size_t ext_offset, std::uint64_t available,
                                                     std::uint32_t ext_word_address) noexcept {
  M68kEaParseResult result{};
  M68kEaMode mode = M68kEaMode::unused;
  std::uint8_t reg = reg3;
  switch (mode3) {
  case 0U: mode = M68kEaMode::data_register; break;
  case 1U: mode = M68kEaMode::address_register; break;
  case 2U: mode = M68kEaMode::address_indirect; break;
  case 3U: mode = M68kEaMode::address_postinc; break;
  case 4U: mode = M68kEaMode::address_predec; break;
  case 5U: mode = M68kEaMode::address_disp16; break;
  case 6U: mode = M68kEaMode::address_index8; break;  // SEG-007-T120: brief-format (d8,An,Xn)
  case 7U:
    switch (reg3) {
    case 0U: mode = M68kEaMode::absolute_word; reg = 0U; break;
    case 1U: mode = M68kEaMode::absolute_long; reg = 0U; break;
    case 2U: mode = M68kEaMode::pc_disp16; reg = 0U; break;
    case 3U: mode = M68kEaMode::pc_index8; reg = 0U; break;  // SEG-007-T124: (d8,PC,Xn)
    case 4U: mode = M68kEaMode::immediate; reg = 0U; break;
    default: return result;  // reserved: out of scope, always illegal_mode
    }
    break;
  default: return result;
  }
  if ((legal & m68k_ea_bit_for(mode)) == 0U) {
    result.status = M68kEaParseStatus::not_permitted;
    return result;
  }
  std::uint32_t ext_bytes = 0U;
  switch (mode) {
  case M68kEaMode::address_disp16:
  case M68kEaMode::pc_disp16:
  case M68kEaMode::address_index8:  // SEG-007-T120: exactly one brief-format extension word
  case M68kEaMode::pc_index8:       // SEG-007-T124: exactly one brief-format extension word
  case M68kEaMode::absolute_word: ext_bytes = 2U; break;
  case M68kEaMode::absolute_long: ext_bytes = 4U; break;
  case M68kEaMode::immediate: ext_bytes = size == M68kMemoryAccessWidth::long_word ? 4U : 2U; break;
  default: ext_bytes = 0U; break;
  }
  result.extension_bytes = ext_bytes;
  if (available < ext_bytes) {
    result.status = M68kEaParseStatus::truncated;
    return result;
  }
  M68kEffectiveAddress ea{};
  ea.mode = mode;
  ea.reg = reg;
  ea.extension_words = static_cast<std::uint8_t>(ext_bytes / 2U);
  const auto read16 = [&](std::size_t at) -> std::uint16_t {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(image[at]) << 8U) | image[at + 1U]);
  };
  switch (mode) {
  case M68kEaMode::address_disp16:
    ea.displacement = static_cast<std::int16_t>(read16(ext_offset));
    break;
  case M68kEaMode::address_index8: {
    // SEG-007-T120: brief-format extension word only. Motorola M68000 Family
    // Programmer's Reference Manual: bit15 D/A, bits14-12 index register,
    // bit11 W/L index size, bits10-9 scale, bit8 brief(0)/full(1) format,
    // bits7-0 signed 8-bit displacement. On a base MC68000 there is no
    // scale and no full format: reject a nonzero scale or full-format bit
    // as an unsupported form (fail closed) rather than silently ignoring it.
    const auto ext = read16(ext_offset);
    if ((ext & UINT16_C(0x0700)) != 0U) {
      result.status = M68kEaParseStatus::illegal_mode;
      return result;
    }
    ea.index_reg = static_cast<std::uint8_t>((ext >> 12U) & 0x7U);
    ea.index_is_address = (ext & UINT16_C(0x8000)) != 0U;
    ea.index_is_long = (ext & UINT16_C(0x0800)) != 0U;
    ea.displacement = static_cast<std::int16_t>(static_cast<std::int8_t>(ext & 0xFFU));
    break;
  }
  case M68kEaMode::pc_disp16: {
    const auto disp = static_cast<std::int16_t>(read16(ext_offset));
    ea.displacement = disp;
    ea.absolute_address = static_cast<std::uint32_t>(static_cast<std::int64_t>(ext_word_address) + disp);
    ea.pc_base_address = ext_word_address;
    break;
  }
  case M68kEaMode::pc_index8: {
    // SEG-007-T124 / ADR-0009: identical brief-format extension-word shape
    // as address_index8 (SEG-007-T120), but the base is PC (the address of
    // this extension word, per `pc_base_address`) rather than An. A nonzero
    // scale or full-format bit is rejected exactly like the An-indexed form;
    // this EA is never statically foldable (see
    // m68k_is_statically_foldable_control_ea), so no `absolute_address` is
    // ever computed here.
    const auto ext = read16(ext_offset);
    if ((ext & UINT16_C(0x0700)) != 0U) {
      result.status = M68kEaParseStatus::illegal_mode;
      return result;
    }
    ea.index_reg = static_cast<std::uint8_t>((ext >> 12U) & 0x7U);
    ea.index_is_address = (ext & UINT16_C(0x8000)) != 0U;
    ea.index_is_long = (ext & UINT16_C(0x0800)) != 0U;
    ea.displacement = static_cast<std::int16_t>(static_cast<std::int8_t>(ext & 0xFFU));
    ea.pc_base_address = ext_word_address;
    break;
  }
  case M68kEaMode::absolute_word:
    ea.absolute_address = static_cast<std::uint32_t>(
        static_cast<std::int32_t>(static_cast<std::int16_t>(read16(ext_offset))));
    break;
  case M68kEaMode::absolute_long:
    ea.absolute_address = (static_cast<std::uint32_t>(image[ext_offset]) << 24U) |
                          (static_cast<std::uint32_t>(image[ext_offset + 1U]) << 16U) |
                          (static_cast<std::uint32_t>(image[ext_offset + 2U]) << 8U) | image[ext_offset + 3U];
    break;
  case M68kEaMode::immediate:
    if (size == M68kMemoryAccessWidth::byte) {
      ea.immediate_value = image[ext_offset + 1U];  // low-order byte of the extension word
    } else if (size == M68kMemoryAccessWidth::word) {
      ea.immediate_value = read16(ext_offset);
    } else {
      ea.immediate_value = (static_cast<std::uint32_t>(read16(ext_offset)) << 16U) | read16(ext_offset + 2U);
    }
    break;
  default: break;
  }
  result.status = M68kEaParseStatus::ok;
  result.ea = ea;
  return result;
}

struct M68kEaFieldOutcome {
  bool ok{};
  M68kEffectiveAddress ea{};
  std::uint32_t extension_bytes{};
  M68kDecodeResult failure{};  // meaningful only when !ok
};

// Parses one EA operand at `consumed_before` bytes past the primary word
// (0 for a single-EA instruction's only operand, or MOVE's source; the
// source's own `extension_bytes` for MOVE's destination -- source-then-
// destination word order, per the contract). Builds the exact same
// decode-stage rejection shape `reject_word` already uses on any failure:
// illegal-for-instruction-EA and truncation both retain only the verified
// two-byte primary provenance, never a fabricated complete span.
[[nodiscard]] M68kEaFieldOutcome m68k_decode_one_ea(const DecodeSource &source, std::span<const std::uint8_t> image,
                                                     std::size_t primary_offset, std::uint64_t total_available,
                                                     const std::array<std::uint8_t, 2> &primary_bytes,
                                                     std::uint32_t consumed_before, std::uint8_t mode3,
                                                     std::uint8_t reg3, M68kEaLegalMask legal,
                                                     M68kMemoryAccessWidth size) {
  M68kEaFieldOutcome outcome{};
  const auto ext_offset = primary_offset + 2U + consumed_before;
  const auto consumed_total_before = 2ULL + consumed_before;
  const auto ext_available = total_available > consumed_total_before ? total_available - consumed_total_before : 0ULL;
  const auto ext_word_address = static_cast<std::uint32_t>(source.address.value + 2U + consumed_before);
  const auto parsed = m68k_parse_ea_field(mode3, reg3, legal, size, image, ext_offset, ext_available, ext_word_address);
  if (parsed.status == M68kEaParseStatus::illegal_mode || parsed.status == M68kEaParseStatus::not_permitted) {
    auto rejected = reject(DecodeOutcome::valid_but_unsupported_instruction, source, total_available, 2U, true);
    rejected.provenance = {source, primary_bytes, ByteLength{2}};
    rejected.has_provenance = true;
    rejected.unsupported_instruction_form = true;
    outcome.failure = rejected;
    return outcome;
  }
  if (parsed.status == M68kEaParseStatus::truncated) {
    const auto requested = static_cast<std::uint32_t>(consumed_total_before + parsed.extension_bytes);
    auto rejected = reject(DecodeOutcome::truncated_instruction, source, total_available, requested, true);
    rejected.provenance = {source, primary_bytes, ByteLength{2}};
    rejected.has_provenance = true;
    outcome.failure = rejected;
    return outcome;
  }
  outcome.ok = true;
  outcome.ea = parsed.ea;
  outcome.extension_bytes = parsed.extension_bytes;
  return outcome;
}

[[nodiscard]] M68kMemoryAccessWidth m68k_size_from_tst_clr_field(std::uint8_t ss) noexcept {
  return ss == 0U ? M68kMemoryAccessWidth::byte : ss == 1U ? M68kMemoryAccessWidth::word : M68kMemoryAccessWidth::long_word;
}

[[nodiscard]] std::optional<M68kMemoryAccessWidth> m68k_size_from_move_field(std::uint8_t ss) noexcept {
  if (ss == 1U) return M68kMemoryAccessWidth::byte;
  if (ss == 3U) return M68kMemoryAccessWidth::word;
  if (ss == 2U) return M68kMemoryAccessWidth::long_word;
  return std::nullopt;
}

[[nodiscard]] M68kDecodedInstruction m68k_finish_general_decode(
    const DecodeSource &source, std::span<const std::uint8_t> image, std::size_t offset,
    const std::array<std::uint8_t, 2> &bytes, M68kInstructionKind kind, M68kMemoryAccessWidth size,
    M68kEffectiveAddress source_ea, M68kEffectiveAddress destination_ea, std::uint32_t total_extension_bytes) {
  M68kDecodedInstruction instruction{};
  const auto total_len = static_cast<std::uint32_t>(2U + total_extension_bytes);
  instruction.provenance = {source, bytes, ByteLength{total_len}};
  instruction.kind = kind;
  instruction.size = size;
  instruction.source_ea = source_ea;
  instruction.destination_ea = destination_ea;
  instruction.raw_bytes.assign(image.begin() + static_cast<std::ptrdiff_t>(offset),
                               image.begin() + static_cast<std::ptrdiff_t>(offset + total_len));
  return instruction;
}

// TST.B/W/L (contract: legal operand set excludes An/d16(PC)/immediate on
// the base MC68000). The caller has already matched the fixed `0100 1010`
// prefix (excluding TAS/ILLEGAL's size field 11) and the one pre-existing
// exact TST.L absolute-long word; every other primary in that family reaches
// here.
[[nodiscard]] M68kDecodeResult m68k_decode_general_tst(const DecodeSource &source, std::span<const std::uint8_t> image,
                                                        std::size_t offset, std::uint64_t available,
                                                        const std::array<std::uint8_t, 2> &bytes, std::uint16_t word) {
  const auto size = m68k_size_from_tst_clr_field(static_cast<std::uint8_t>((word >> 6U) & 0x3U));
  const auto mode3 = static_cast<std::uint8_t>((word >> 3U) & 0x7U);
  const auto reg3 = static_cast<std::uint8_t>(word & 0x7U);
  const auto src = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, mode3, reg3, m68k_ea_tst_operand, size);
  if (!src.ok) return src.failure;
  return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::tst, size, src.ea, {},
                                     src.extension_bytes);
}

// MOVE.B/W/L and MOVEA.W/L share one primary-word shape: bits15-14==00,
// bits13-12==SIZE, bits11-6==destination (reg-then-mode), bits5-0==source
// (mode-then-reg). Destination mode field 001 (address-register-direct) is
// structurally MOVEA -- except for byte size, where mode-001 destination is
// simply an illegal MOVE.B destination (An not in the data-alterable set),
// which the shared destination-EA legality check below rejects naturally
// with no separate MOVEA-byte special case (contract: "the 'byte MOVEA' bit
// pattern is not a reserved/illegal MOVEA form, it is simply an illegal
// MOVE.B destination").
[[nodiscard]] std::optional<M68kDecodeResult> m68k_decode_general_move(
    const DecodeSource &source, std::span<const std::uint8_t> image, std::size_t offset, std::uint64_t available,
    const std::array<std::uint8_t, 2> &bytes, std::uint16_t word) {
  if ((word & 0xC000U) != 0x0000U) return std::nullopt;
  const auto size_opt = m68k_size_from_move_field(static_cast<std::uint8_t>((word >> 12U) & 0x3U));
  if (!size_opt) return std::nullopt;  // size field 00: not a MOVE/MOVEA primary at all
  const auto size = *size_opt;
  const auto src_mode3 = static_cast<std::uint8_t>((word >> 3U) & 0x7U);
  const auto src_reg3 = static_cast<std::uint8_t>(word & 0x7U);
  const auto dst_mode3 = static_cast<std::uint8_t>((word >> 6U) & 0x7U);
  const auto dst_reg3 = static_cast<std::uint8_t>((word >> 9U) & 0x7U);
  if (dst_mode3 == 1U && size != M68kMemoryAccessWidth::byte) {
    const auto src = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, src_mode3, src_reg3,
                                         m68k_ea_move_family_source, size);
    if (!src.ok) return src.failure;
    const M68kEffectiveAddress destination{M68kEaMode::address_register, dst_reg3, 0, 0, 0, 0};
    return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::movea, size, src.ea,
                                       destination, src.extension_bytes);
  }
  const auto src = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, src_mode3, src_reg3,
                                       m68k_ea_move_family_source, size);
  if (!src.ok) return src.failure;
  if (size == M68kMemoryAccessWidth::byte && src.ea.mode == M68kEaMode::address_register) {
    // "For byte size operation, address register direct is not allowed"
    // (contract, MOVE source table footnote).
    auto rejected = reject(DecodeOutcome::valid_but_unsupported_instruction, source, available, 2U, true);
    rejected.provenance = {source, bytes, ByteLength{2}};
    rejected.has_provenance = true;
    rejected.unsupported_instruction_form = true;
    return rejected;
  }
  const auto dst = m68k_decode_one_ea(source, image, offset, available, bytes, src.extension_bytes, dst_mode3,
                                       dst_reg3, m68k_ea_move_family_destination, size);
  if (!dst.ok) return dst.failure;
  return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::move, size, src.ea, dst.ea,
                                     src.extension_bytes + dst.extension_bytes);
}

// SEG-007-T088: MOVE <ea>,SR (0100 0110 11 mmm rrr, 0x46C0-0x46FF), a
// privileged System Control Group instruction, word-size only, per the
// public Motorola M68000 Family Programmer's Reference Manual (1988, `M1`,
// already cited by docs/references/genesis-rom-startup-contract.md). Legal
// source EA set is `m68k_ea_move_to_sr_source` (see its own doc comment for
// the exact encoding/scope justification -- Dn and #imm only).
[[nodiscard]] std::optional<M68kDecodeResult> m68k_decode_general_move_to_sr(
    const DecodeSource &source, std::span<const std::uint8_t> image, std::size_t offset, std::uint64_t available,
    const std::array<std::uint8_t, 2> &bytes, std::uint16_t word) {
  if ((word & 0xFFC0U) != 0x46C0U) return std::nullopt;
  const auto mode3 = static_cast<std::uint8_t>((word >> 3U) & 0x7U);
  const auto reg3 = static_cast<std::uint8_t>(word & 0x7U);
  const auto src = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, mode3, reg3,
                                       m68k_ea_move_to_sr_source, M68kMemoryAccessWidth::word);
  if (!src.ok) return src.failure;
  return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::move_to_sr,
                                     M68kMemoryAccessWidth::word, src.ea, {}, src.extension_bytes);
}

// SEG-007-T116: MOVE from SR (0100 0000 11 mmm rrr, 0x40C0-0x40FF), a
// System Control Group instruction, word-size only, per the public Motorola
// M68000 Family Programmer's Reference Manual (1988, `M1`). On the original
// MC68000 it is unprivileged and affects no condition codes. Legal
// destination EA set is `m68k_ea_move_from_sr_destination` (see its own doc
// comment -- data-register-direct only in this project). `destination_ea`
// carries the decoded operand (matching the `clr` convention); there is no
// `source_ea` (the source is the fixed SR).
[[nodiscard]] std::optional<M68kDecodeResult> m68k_decode_general_move_from_sr(
    const DecodeSource &source, std::span<const std::uint8_t> image, std::size_t offset, std::uint64_t available,
    const std::array<std::uint8_t, 2> &bytes, std::uint16_t word) {
  if ((word & 0xFFC0U) != 0x40C0U) return std::nullopt;
  const auto mode3 = static_cast<std::uint8_t>((word >> 3U) & 0x7U);
  const auto reg3 = static_cast<std::uint8_t>(word & 0x7U);
  const auto dst = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, mode3, reg3,
                                       m68k_ea_move_from_sr_destination, M68kMemoryAccessWidth::word);
  if (!dst.ok) return dst.failure;
  return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::move_from_sr,
                                     M68kMemoryAccessWidth::word, {}, dst.ea, dst.extension_bytes);
}

// SEG-007-T118: MOVE <ea>,CCR (0100 0100 11 mmm rrr, 0x44C0-0x44FF), a
// System Control Group instruction, word-size only, per the public Motorola
// M68000 Family Programmer's Reference Manual (1988, `M1`). It is unprivileged
// on every 68000-family part and affects all condition codes; the source word
// is read and only its low-order byte updates the CCR. Legal source EA set is
// `m68k_ea_move_to_ccr_source` (see its own doc comment -- data-register-direct
// only in this project). `source_ea` carries the decoded operand; there is no
// `destination_ea` (the destination is the implied CCR).
[[nodiscard]] std::optional<M68kDecodeResult> m68k_decode_general_move_to_ccr(
    const DecodeSource &source, std::span<const std::uint8_t> image, std::size_t offset, std::uint64_t available,
    const std::array<std::uint8_t, 2> &bytes, std::uint16_t word) {
  if ((word & 0xFFC0U) != 0x44C0U) return std::nullopt;
  const auto mode3 = static_cast<std::uint8_t>((word >> 3U) & 0x7U);
  const auto reg3 = static_cast<std::uint8_t>(word & 0x7U);
  const auto src = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, mode3, reg3,
                                       m68k_ea_move_to_ccr_source, M68kMemoryAccessWidth::word);
  if (!src.ok) return src.failure;
  return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::move_to_ccr,
                                     M68kMemoryAccessWidth::word, src.ea, {}, src.extension_bytes);
}

[[nodiscard]] std::optional<M68kDecodeResult> m68k_decode_general_clr(
    const DecodeSource &source, std::span<const std::uint8_t> image, std::size_t offset, std::uint64_t available,
    const std::array<std::uint8_t, 2> &bytes, std::uint16_t word) {
  if ((word & 0xFF00U) != 0x4200U || (word & 0xC0U) == 0xC0U) return std::nullopt;
  const auto size = m68k_size_from_tst_clr_field(static_cast<std::uint8_t>((word >> 6U) & 0x3U));
  const auto mode3 = static_cast<std::uint8_t>((word >> 3U) & 0x7U);
  const auto reg3 = static_cast<std::uint8_t>(word & 0x7U);
  const auto dst = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, mode3, reg3, m68k_ea_clr_not_operand, size);
  if (!dst.ok) return dst.failure;
  return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::clr, size, {}, dst.ea,
                                     dst.extension_bytes);
}

// SEG-007-T168: NOT <ea> (0100 0110 ss mmmrrr, 0x4600-0x46FF), M68000PM/AD
// Rev. 1 §4 "NOT" entry -- same word shape as CLR (0x4200-0x42FF) with the
// operation-select field 0110 instead of 0010, same size field, same legal
// destination EA set (`m68k_ea_data_alterable`), same reserved size==11
// exclusion.
[[nodiscard]] std::optional<M68kDecodeResult> m68k_decode_general_not(
    const DecodeSource &source, std::span<const std::uint8_t> image, std::size_t offset, std::uint64_t available,
    const std::array<std::uint8_t, 2> &bytes, std::uint16_t word) {
  if ((word & 0xFF00U) != 0x4600U || (word & 0xC0U) == 0xC0U) return std::nullopt;
  const auto size = m68k_size_from_tst_clr_field(static_cast<std::uint8_t>((word >> 6U) & 0x3U));
  const auto mode3 = static_cast<std::uint8_t>((word >> 3U) & 0x7U);
  const auto reg3 = static_cast<std::uint8_t>(word & 0x7U);
  const auto dst = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, mode3, reg3, m68k_ea_clr_not_operand, size);
  if (!dst.ok) return dst.failure;
  return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::not_operand, size, {}, dst.ea,
                                     dst.extension_bytes);
}

// NEG <ea> (0100 0100 ss mmmrrr, M68000PM/AD Rev. 1 §4).  Like CLR and NOT,
// NEG accepts byte/word/long sizes (never reserved size 11) and the shared
// data-alterable destination set.  Keep the historical function name: its
// former NEG.W Dn subset is now decoded by this general path.
[[nodiscard]] std::optional<M68kDecodeResult> m68k_decode_general_negate_word(
    const DecodeSource &source, std::span<const std::uint8_t> image, std::size_t offset, std::uint64_t available,
    const std::array<std::uint8_t, 2> &bytes, std::uint16_t word) {
  if ((word & UINT16_C(0xFF00)) != UINT16_C(0x4400) || (word & UINT16_C(0x00C0)) == UINT16_C(0x00C0))
    return std::nullopt;
  const auto size = m68k_size_from_tst_clr_field(static_cast<std::uint8_t>((word >> 6U) & 0x3U));
  const auto mode3 = static_cast<std::uint8_t>((word >> 3U) & 0x7U);
  const auto reg3 = static_cast<std::uint8_t>(word & 0x7U);
  const auto dst = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, mode3, reg3,
                                       m68k_ea_data_alterable, size);
  if (!dst.ok) return dst.failure;
  return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::negate_word, size, {},
                                    dst.ea, dst.extension_bytes);
}

// M68000PM/AD Rev. 1, §2 and §4 CMP/CMPI/CMPA entries.  These use the
// existing general EA decoder; no compare-specific extension parser exists.
[[nodiscard]] std::optional<M68kDecodeResult> m68k_decode_general_compare(
    const DecodeSource &source, std::span<const std::uint8_t> image, std::size_t offset, std::uint64_t available,
    const std::array<std::uint8_t, 2> &bytes, std::uint16_t word) {
  const auto mode = static_cast<std::uint8_t>((word >> 3U) & 7U);
  const auto reg = static_cast<std::uint8_t>(word & 7U);
  const auto destination = static_cast<std::uint8_t>((word >> 9U) & 7U);
  const auto opmode = static_cast<std::uint8_t>((word >> 6U) & 7U);
  if ((word & 0xF000U) == 0xB000U && (opmode <= 2U || opmode == 3U || opmode == 7U)) {
    const bool address_compare = opmode == 3U || opmode == 7U;
    const auto size = address_compare ? (opmode == 3U ? M68kMemoryAccessWidth::word : M68kMemoryAccessWidth::long_word) :
        m68k_size_from_tst_clr_field(opmode);
    const auto src = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, mode, reg,
                                         m68k_ea_add_sub_cmp_source, size);
    if (!src.ok) return src.failure;
    if (!address_compare && size == M68kMemoryAccessWidth::byte && src.ea.mode == M68kEaMode::address_register) {
      auto rejected = reject(DecodeOutcome::valid_but_unsupported_instruction, source, available, 2U, true);
      rejected.provenance = {source, bytes, ByteLength{2}}; rejected.has_provenance = true;
      rejected.unsupported_instruction_form = true; return rejected;
    }
    const M68kEffectiveAddress dst{address_compare ? M68kEaMode::address_register : M68kEaMode::data_register,
                                    destination, 0, 0, 0, 0};
    auto decoded = m68k_finish_general_decode(source, image, offset, bytes,
                                               address_compare ? M68kInstructionKind::cmpa : M68kInstructionKind::cmp,
                                               size, src.ea, dst,
                                               src.extension_bytes);
    return decoded;
  }
  if ((word & 0xFF00U) != 0x0C00U || (word & 0xC0U) == 0xC0U) return std::nullopt;
  const auto size = m68k_size_from_tst_clr_field(static_cast<std::uint8_t>((word >> 6U) & 3U));
  const auto imm = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, 7U, 4U, m68k_ea_immediate, size);
  if (!imm.ok) return imm.failure;
  const auto dst = m68k_decode_one_ea(source, image, offset, available, bytes, imm.extension_bytes, mode, reg,
                                       m68k_ea_data_alterable_with_index, size);
  if (!dst.ok) return dst.failure;
  auto decoded = m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::cmpi, size, imm.ea, dst.ea,
                                             imm.extension_bytes + dst.extension_bytes);
  return decoded;
}

// M68000PM/AD Rev. 1, §4 SUB/SUBA/SUBI/SUBQ entries.  Every extension is
// consumed through the shared EA decoder, preserving its bounds/provenance
// behavior and the selected forms' data-alterable destination restriction.
[[nodiscard]] std::optional<M68kDecodeResult> m68k_decode_general_subtract(
    const DecodeSource &source, std::span<const std::uint8_t> image, std::size_t offset, std::uint64_t available,
    const std::array<std::uint8_t, 2> &bytes, std::uint16_t word) {
  const auto mode = static_cast<std::uint8_t>((word >> 3U) & 7U);
  const auto reg = static_cast<std::uint8_t>(word & 7U);
  const auto destination = static_cast<std::uint8_t>((word >> 9U) & 7U);
  const auto opmode = static_cast<std::uint8_t>((word >> 6U) & 7U);
  if ((word & 0xF000U) == 0x9000U) {
    if (opmode <= 2U || opmode == 3U || opmode == 7U) {
      const bool address_subtract = opmode == 3U || opmode == 7U;
      const auto size = address_subtract ? (opmode == 3U ? M68kMemoryAccessWidth::word : M68kMemoryAccessWidth::long_word)
                                         : m68k_size_from_tst_clr_field(opmode);
      const auto src = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, mode, reg,
                                           m68k_ea_add_sub_cmp_source, size);
      if (!src.ok) return src.failure;
      if (!address_subtract && size == M68kMemoryAccessWidth::byte && src.ea.mode == M68kEaMode::address_register) {
        auto rejected = reject(DecodeOutcome::valid_but_unsupported_instruction, source, available, 2U, true);
        rejected.provenance = {source, bytes, ByteLength{2}}; rejected.has_provenance = true;
        rejected.unsupported_instruction_form = true; return rejected;
      }
      return m68k_finish_general_decode(source, image, offset, bytes,
          address_subtract ? M68kInstructionKind::suba : M68kInstructionKind::sub, size, src.ea,
          {address_subtract ? M68kEaMode::address_register : M68kEaMode::data_register, destination, 0, 0, 0, 0},
          src.extension_bytes);
    }
    if (opmode >= 4U && opmode <= 6U) {
      const auto size = m68k_size_from_tst_clr_field(static_cast<std::uint8_t>(opmode - 4U));
      const M68kEffectiveAddress src{M68kEaMode::data_register, destination, 0, 0, 0, 0};
      const auto dst = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, mode, reg,
                                           m68k_ea_reverse_arithmetic_destination, size);
      if (!dst.ok) return dst.failure;
      return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::sub, size, src, dst.ea,
                                        dst.extension_bytes);
    }
  }
  if ((word & 0xFF00U) == 0x0400U && (word & 0xC0U) != 0xC0U) {
    const auto size = m68k_size_from_tst_clr_field(static_cast<std::uint8_t>((word >> 6U) & 3U));
    const auto imm = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, 7U, 4U, m68k_ea_immediate, size);
    if (!imm.ok) return imm.failure;
    const auto dst = m68k_decode_one_ea(source, image, offset, available, bytes, imm.extension_bytes, mode, reg,
                                         m68k_ea_data_alterable_with_index, size);
    if (!dst.ok) return dst.failure;
    return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::subi, size, imm.ea, dst.ea,
                                      imm.extension_bytes + dst.extension_bytes);
  }
  // Bit 8 distinguishes SUBQ (1) from ADDQ (0); do not accidentally select
  // ADDQ through this bounded subtraction batch.
  if ((word & 0xF100U) == 0x5100U && (word & 0xC0U) != 0xC0U) {
    const auto size = m68k_size_from_tst_clr_field(static_cast<std::uint8_t>((word >> 6U) & 3U));
    if (mode == 1U && size == M68kMemoryAccessWidth::byte) return std::nullopt;
    const auto legal = mode == 1U ? m68k_ea_an : m68k_ea_addq_subq_destination;
    const auto dst = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, mode, reg, legal, size);
    if (!dst.ok) return dst.failure;
    auto decoded = m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::subq, size, {}, dst.ea,
                                               dst.extension_bytes);
    decoded.source_ea = {M68kEaMode::immediate, 0, 0, 0, destination == 0U ? 8U : destination, 0};
    return decoded;
  }
  return std::nullopt;
}

// M68000PM/AD Rev. 1, §4 ADD/ADDA/ADDI/ADDQ entries.  This mirrors the
// already-selected subtraction shapes and delegates every extension and EA
// legality decision to the shared decoder.
[[nodiscard]] std::optional<M68kDecodeResult> m68k_decode_general_add(
    const DecodeSource &source, std::span<const std::uint8_t> image, std::size_t offset, std::uint64_t available,
    const std::array<std::uint8_t, 2> &bytes, std::uint16_t word) {
  const auto mode = static_cast<std::uint8_t>((word >> 3U) & 7U);
  const auto reg = static_cast<std::uint8_t>(word & 7U);
  const auto destination = static_cast<std::uint8_t>((word >> 9U) & 7U);
  const auto opmode = static_cast<std::uint8_t>((word >> 6U) & 7U);
  if ((word & 0xF000U) == 0xD000U) {
    if (opmode <= 2U || opmode == 3U || opmode == 7U) {
      const bool address_add = opmode == 3U || opmode == 7U;
      const auto size = address_add ? (opmode == 3U ? M68kMemoryAccessWidth::word : M68kMemoryAccessWidth::long_word)
                                    : m68k_size_from_tst_clr_field(opmode);
      // ADD.B excludes An, while ADD.W/L accept it. ADDA remains a distinct
      // instruction family because its destination and CCR semantics differ.
      const auto source_legal = address_add
          ? m68k_ea_add_sub_cmp_source
          : (size == M68kMemoryAccessWidth::byte
                 ? static_cast<M68kEaLegalMask>(m68k_ea_add_sub_cmp_source & ~m68k_ea_an)
                 : m68k_ea_add_sub_cmp_source);
      const auto src = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, mode, reg,
                                           source_legal, size);
      if (!src.ok) return src.failure;
      return m68k_finish_general_decode(source, image, offset, bytes,
          address_add ? M68kInstructionKind::adda : M68kInstructionKind::add, size, src.ea,
          {address_add ? M68kEaMode::address_register : M68kEaMode::data_register, destination, 0, 0, 0, 0},
          src.extension_bytes);
    }
    if (opmode >= 4U && opmode <= 6U) {
      const auto size = m68k_size_from_tst_clr_field(static_cast<std::uint8_t>(opmode - 4U));
      const M68kEffectiveAddress src{M68kEaMode::data_register, destination, 0, 0, 0, 0};
      const auto dst = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, mode, reg,
                                           m68k_ea_reverse_arithmetic_destination, size);
      if (!dst.ok) return dst.failure;
      return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::add, size, src, dst.ea,
                                        dst.extension_bytes);
    }
  }
  if ((word & 0xFF00U) == 0x0600U && (word & 0xC0U) != 0xC0U) {
    const auto size = m68k_size_from_tst_clr_field(static_cast<std::uint8_t>((word >> 6U) & 3U));
    const auto imm = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, 7U, 4U, m68k_ea_immediate, size);
    if (!imm.ok) return imm.failure;
    const auto dst = m68k_decode_one_ea(source, image, offset, available, bytes, imm.extension_bytes, mode, reg,
                                         m68k_ea_data_alterable_with_index, size);
    if (!dst.ok) return dst.failure;
    return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::addi, size, imm.ea, dst.ea,
                                      imm.extension_bytes + dst.extension_bytes);
  }
  if ((word & 0xF100U) == 0x5000U && (word & 0xC0U) != 0xC0U) {
    const auto size = m68k_size_from_tst_clr_field(static_cast<std::uint8_t>((word >> 6U) & 3U));
    if (mode == 1U && size == M68kMemoryAccessWidth::byte) return std::nullopt;
    const auto legal = mode == 1U ? m68k_ea_an : m68k_ea_addq_subq_destination;
    const auto dst = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, mode, reg, legal, size);
    if (!dst.ok) return dst.failure;
    auto decoded = m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::addq, size, {}, dst.ea,
                                               dst.extension_bytes);
    decoded.source_ea = {M68kEaMode::immediate, 0, 0, 0, destination == 0U ? 8U : destination, 0};
    return decoded;
  }
  return std::nullopt;
}

// M68000PM/AD Rev. 1, §4 AND/ANDI/OR/ORI/EOR/EORI.  The immediate CCR/SR
// encodings are rejected before any EA extension is parsed: they are system
// operations, not ordinary immediate data operations in this batch.
[[nodiscard]] std::optional<M68kDecodeResult> m68k_decode_general_logical(
    const DecodeSource &source, std::span<const std::uint8_t> image, std::size_t offset, std::uint64_t available,
    const std::array<std::uint8_t, 2> &bytes, std::uint16_t word) {
  const auto mode = static_cast<std::uint8_t>((word >> 3U) & 7U);
  const auto reg = static_cast<std::uint8_t>(word & 7U);
  const auto destination = static_cast<std::uint8_t>((word >> 9U) & 7U);
  const auto opmode = static_cast<std::uint8_t>((word >> 6U) & 7U);
  const auto ordinary_immediate = [&](std::uint16_t primary, M68kInstructionKind kind) -> std::optional<M68kDecodeResult> {
    if ((word & 0xFF00U) != primary || (word & 0xC0U) == 0xC0U) return std::nullopt;
    const auto size = m68k_size_from_tst_clr_field(static_cast<std::uint8_t>((word >> 6U) & 3U));
    const auto imm = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, 7U, 4U, m68k_ea_immediate, size);
    if (!imm.ok) return imm.failure;
    const auto dst = m68k_decode_one_ea(source, image, offset, available, bytes, imm.extension_bytes, mode, reg,
                                        m68k_ea_logical_immediate_destination, size);
    if (!dst.ok) return dst.failure;
    return m68k_finish_general_decode(source, image, offset, bytes, kind, size, imm.ea, dst.ea,
                                      imm.extension_bytes + dst.extension_bytes);
  };
  // Exact reserved immediate-to-CCR/SR words must not consume an extension.
  if (word == 0x023CU || word == 0x027CU || word == 0x003CU || word == 0x007CU ||
      word == 0x0A3CU || word == 0x0A7CU)
    return std::nullopt;
  if (auto decoded = ordinary_immediate(0x0200U, M68kInstructionKind::andi)) return decoded;
  if (auto decoded = ordinary_immediate(0x0000U, M68kInstructionKind::ori)) return decoded;
  if (auto decoded = ordinary_immediate(0x0A00U, M68kInstructionKind::eori)) return decoded;
  const auto decode_register_logical = [&](std::uint16_t family, M68kInstructionKind kind,
                                            bool eor_only) -> std::optional<M68kDecodeResult> {
    if ((word & 0xF000U) != family || (eor_only ? (opmode < 4U || opmode > 6U)
                                                   : (opmode == 3U || opmode == 7U))) return std::nullopt;
    if (opmode <= 2U) {
      if (eor_only) return std::nullopt;
      const auto size = m68k_size_from_tst_clr_field(opmode);
      // SEG-007-T198 introduced word-size `(d8,PC,Xn)`; SEG-021-T007 widened to the full manual set.
      // SEG-021-T007: the full manual source set applies to every size (byte/long `(d8,PC,Xn)` included).
      const auto source_legal = m68k_ea_and_or_source;
      const auto src = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, mode, reg, source_legal, size);
      if (!src.ok) return src.failure;
      return m68k_finish_general_decode(source, image, offset, bytes, kind, size, src.ea,
          {M68kEaMode::data_register, destination, 0, 0, 0, 0}, src.extension_bytes);
    }
    const auto size = m68k_size_from_tst_clr_field(static_cast<std::uint8_t>(opmode - 4U));
    const M68kEffectiveAddress src{M68kEaMode::data_register, destination, 0, 0, 0, 0};
    const auto legal = eor_only ? m68k_ea_eor_destination : m68k_ea_and_or_reverse_destination;
    const auto dst = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, mode, reg, legal, size);
    if (!dst.ok) return dst.failure;
    return m68k_finish_general_decode(source, image, offset, bytes, kind, size, src, dst.ea, dst.extension_bytes);
  };
  if (auto decoded = decode_register_logical(0xC000U, M68kInstructionKind::logical_and, false)) return decoded;
  if (auto decoded = decode_register_logical(0x8000U, M68kInstructionKind::logical_or, false)) return decoded;
  if (auto decoded = decode_register_logical(0xB000U, M68kInstructionKind::eor, true)) return decoded;
  // SEG-007-T220 / SEG-021-T010: MULS.W <ea>,Dn -- the same 1100-line opcode
  // family as AND above, opmode 111 (`decode_register_logical` explicitly
  // declines opmode 3/7 for every family, so it never reaches that shared
  // body). See M68kInstructionKind::multiply_signed_word's own doc comment
  // for the full opcode/EA-legality/CCR contract; the legal source EA set is
  // `m68k_ea_mul_div_source` (every mode except An-direct, including
  // `(d8,An,Xn)` and `(d8,PC,Xn)`).
  if ((word & 0xF1C0U) == 0xC1C0U) {
    const auto src = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, mode, reg,
                                         m68k_ea_mul_div_source, M68kMemoryAccessWidth::word);
    if (!src.ok) return src.failure;
    return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::multiply_signed_word,
                                       M68kMemoryAccessWidth::word, src.ea,
                                       {M68kEaMode::data_register, destination, 0, 0, 0, 0}, src.extension_bytes);
  }
  // SEG-007-T222 / SEG-021-T010: MULU.W <ea>,Dn -- opmode 011 sibling of
  // MULS.W above, same opcode line (1100). See
  // M68kInstructionKind::multiply_unsigned_word's own doc comment for the
  // full contract.
  if ((word & 0xF1C0U) == 0xC0C0U) {
    const auto src = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, mode, reg,
                                         m68k_ea_mul_div_source, M68kMemoryAccessWidth::word);
    if (!src.ok) return src.failure;
    return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::multiply_unsigned_word,
                                       M68kMemoryAccessWidth::word, src.ea,
                                       {M68kEaMode::data_register, destination, 0, 0, 0, 0}, src.extension_bytes);
  }
  // SEG-007-T222 / SEG-021-T010: DIVS.W <ea>,Dn -- opcode line 1000 (0x8),
  // opmode 111. See M68kInstructionKind::divide_signed_word's own doc
  // comment for the full opcode/EA-legality/CCR/exception contract
  // (ADR-0037).
  if ((word & 0xF1C0U) == 0x81C0U) {
    const auto src = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, mode, reg,
                                         m68k_ea_mul_div_source, M68kMemoryAccessWidth::word);
    if (!src.ok) return src.failure;
    return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::divide_signed_word,
                                       M68kMemoryAccessWidth::word, src.ea,
                                       {M68kEaMode::data_register, destination, 0, 0, 0, 0}, src.extension_bytes);
  }
  // SEG-007-T222 / SEG-021-T010: DIVU.W <ea>,Dn -- opcode line 1000 (0x8),
  // opmode 011. See M68kInstructionKind::divide_unsigned_word's own doc
  // comment.
  if ((word & 0xF1C0U) == 0x80C0U) {
    const auto src = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, mode, reg,
                                         m68k_ea_mul_div_source, M68kMemoryAccessWidth::word);
    if (!src.ok) return src.failure;
    return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::divide_unsigned_word,
                                       M68kMemoryAccessWidth::word, src.ea,
                                       {M68kEaMode::data_register, destination, 0, 0, 0, 0}, src.extension_bytes);
  }
  return std::nullopt;
}

// SEG-007-T025 (Batch C, C1): SWAP Dn, EXT.W Dn, EXT.L Dn. M68000PM/AD Rev.
// 1, §4, SWAP and EXT entries; verified against the pinned Musashi
// m68k_in.c opcode table (SWAP: "0100100001000...", EXT.W:
// "0100100010000...", EXT.L: "0100100011000...", each followed by a 3-bit
// register field, no further extension word). Each mask below is the exact
// 13-bit fixed prefix plus the 3-bit register field only. This cannot widen
// into PEA or MOVEM at the same primary byte: PEA's and MOVEM's own control/
// register-alterable EA legality already excludes their instructions' Dn-
// direct (mode3==000) slot, which is exactly the one slot SWAP/EXT.W/EXT.L
// claim here, so the two decoders never contend for the same bit pattern.
// NBCD is different: it legitimately has a base-MC68000 `NBCD Dn` form
// ("0100100000000...", prefix 0x4800), but that prefix's bit 6 is 0 where
// SWAP/EXT's is 1 (0x4840/0x4880/0x48C0), so NBCD Dn already has its own
// distinct fixed opcode prefix -- it does not collide with this mask for any
// other reason than that pre-existing prefix difference. EXTB.L (68020+,
// "0100100111000...", prefix 0x49) is a different top byte entirely and is
// correctly left unselected. No extension word, memory access, or new EA
// mode is introduced; `destination_ea` carries the sole Dn operand (see
// M68kInstructionKind::swap doc).
[[nodiscard]] std::optional<M68kDecodeResult> m68k_decode_general_swap_ext(
    const DecodeSource &source, std::span<const std::uint8_t> image, std::size_t offset,
    const std::array<std::uint8_t, 2> &bytes, std::uint16_t word) {
  const auto reg = static_cast<std::uint8_t>(word & 0x7U);
  const M68kEffectiveAddress operand{M68kEaMode::data_register, reg, 0, 0, 0, 0};
  if ((word & 0xFFF8U) == 0x4840U)
    return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::swap,
                                       M68kMemoryAccessWidth::long_word, {}, operand, 0U);
  if ((word & 0xFFF8U) == 0x4880U)
    return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::ext_w,
                                       M68kMemoryAccessWidth::word, {}, operand, 0U);
  if ((word & 0xFFF8U) == 0x48C0U)
    return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::ext_l,
                                       M68kMemoryAccessWidth::long_word, {}, operand, 0U);
  return std::nullopt;
}

// SEG-007-T025 (Batch C, C2): PEA <ea>. M68000PM/AD Rev. 1, §4, PEA entry;
// verified against the pinned Musashi m68k_in.c opcode table
// ("0100100001......", allowed-ea legend "A..DXWLdx."). PEA shares the
// primary word 0x4840-0x487F with SWAP's exact Dn-direct (mode3==000) slot,
// which is why this decoder MUST run after m68k_decode_general_swap_ext in
// the dispatch chain: SWAP's mask (0xFFF8, all 8 Dn registers) exhaustively
// claims that entire mode3==000 sub-range first, so by the time this
// decoder's broader 0xFFC0 mask is even reached, no mode3==000 word remains
// for it to see. PEA's legal EA ceiling is exactly the existing
// `m68k_ea_control_modes` set already shared by LEA/JMP/JSR -- the allowed-ea
// legend's A/D/W/L/d entries (An indirect, d16(An), absolute.w, absolute.l,
// d16(PC)) are the identical five modes, and PEA's X/x (indexed) entries
// remain out of the project's T023 EA tranche exactly like LEA/JMP/JSR's.
// Dn/An-direct/postinc/predec/immediate are illegal (absent from the
// legend) and rejected by the same shared EA legality check every other
// control-EA form already uses; no new EA decoder is introduced.
[[nodiscard]] std::optional<M68kDecodeResult> m68k_decode_general_pea(
    const DecodeSource &source, std::span<const std::uint8_t> image, std::size_t offset, std::uint64_t available,
    const std::array<std::uint8_t, 2> &bytes, std::uint16_t word) {
  if ((word & 0xFFC0U) != 0x4840U) return std::nullopt;
  const auto mode3 = static_cast<std::uint8_t>((word >> 3U) & 0x7U);
  const auto reg3 = static_cast<std::uint8_t>(word & 0x7U);
  const auto src = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, mode3, reg3, m68k_ea_control_modes,
                                       M68kMemoryAccessWidth::long_word);
  if (!src.ok) return src.failure;
  // PEA has no destination_ea (its one EA is source_ea, the address to
  // compute and push), mirroring the existing JMP/JSR convention exactly.
  return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::pea,
                                     M68kMemoryAccessWidth::long_word, src.ea, {}, src.extension_bytes);
}

[[nodiscard]] std::optional<M68kDecodeResult> m68k_decode_general_lea(
    const DecodeSource &source, std::span<const std::uint8_t> image, std::size_t offset, std::uint64_t available,
    const std::array<std::uint8_t, 2> &bytes, std::uint16_t word) {
  if ((word & 0xF1C0U) != 0x41C0U) return std::nullopt;
  const auto dest_reg = static_cast<std::uint8_t>((word >> 9U) & 0x7U);
  const auto mode3 = static_cast<std::uint8_t>((word >> 3U) & 0x7U);
  const auto reg3 = static_cast<std::uint8_t>(word & 0x7U);
  // SEG-007-T135 / SEG-007-T215: LEA-only widened control-EA set adds the
  // brief-format `(d8,An,Xn)` indexed base and the brief-format `(d8,PC,Xn)`
  // PC-relative indexed base; PEA and the shared `m68k_ea_control_modes`
  // stay unchanged (ADR-0009 JMP/JSR precedent).
  const auto src = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, mode3, reg3, m68k_ea_lea_control_modes,
                                       M68kMemoryAccessWidth::long_word);
  if (!src.ok) return src.failure;
  const M68kEffectiveAddress destination{M68kEaMode::address_register, dest_reg, 0, 0, 0, 0};
  return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::lea,
                                     M68kMemoryAccessWidth::long_word, src.ea, destination, src.extension_bytes);
}

[[nodiscard]] std::optional<M68kDecodeResult> m68k_decode_general_jmp(
    const DecodeSource &source, std::span<const std::uint8_t> image, std::size_t offset, std::uint64_t available,
    const std::array<std::uint8_t, 2> &bytes, std::uint16_t word) {
  if ((word & 0xFFC0U) != 0x4EC0U) return std::nullopt;
  const auto mode3 = static_cast<std::uint8_t>((word >> 3U) & 0x7U);
  const auto reg3 = static_cast<std::uint8_t>(word & 0x7U);
  // SEG-007-T124 / ADR-0009: JMP's own widened control-EA legal set (adds
  // the brief PC-relative indexed form); LEA/PEA above stay on the
  // unchanged shared `m68k_ea_control_modes`.
  const auto src = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, mode3, reg3,
                                       m68k_ea_jsr_jmp_control_modes, M68kMemoryAccessWidth::long_word);
  if (!src.ok) return src.failure;
  return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::jmp,
                                     M68kMemoryAccessWidth::long_word, src.ea, {}, src.extension_bytes);
}

[[nodiscard]] std::optional<M68kDecodeResult> m68k_decode_general_jsr(
    const DecodeSource &source, std::span<const std::uint8_t> image, std::size_t offset, std::uint64_t available,
    const std::array<std::uint8_t, 2> &bytes, std::uint16_t word) {
  if ((word & 0xFFC0U) != 0x4E80U) return std::nullopt;
  const auto mode3 = static_cast<std::uint8_t>((word >> 3U) & 0x7U);
  const auto reg3 = static_cast<std::uint8_t>(word & 0x7U);
  // SEG-007-T124 / ADR-0009: JSR's own widened control-EA legal set,
  // identical to JMP's above.
  const auto src = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, mode3, reg3,
                                       m68k_ea_jsr_jmp_control_modes, M68kMemoryAccessWidth::long_word);
  if (!src.ok) return src.failure;
  return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::jsr,
                                     M68kMemoryAccessWidth::long_word, src.ea, {}, src.extension_bytes);
}

// SEG-007-T025 (Batch C, C2): LINK An,#<word displacement> and UNLK An. Base
// MC68000 forms only. M68000PM/AD Rev. 1, §4, LINK and UNLK entries;
// verified against the pinned Musashi m68k_in.c opcode table: LINK.W is
// "0100111001010..." (0x4E50|An, one signed 16-bit extension word) and UNLK
// is "0100111001011..." (0x4E58|An, no extension word). The 68020+
// long-displacement LINK.L form ("0100100000001...", 0x4808|An) is a
// different primary word entirely and is correctly never selected here.
// `destination_ea` carries the An operand for both (read-then-written, same
// convention as SWAP/EXT); LINK's `source_ea` additionally carries its
// decoded displacement as an ordinary immediate EA fact (raw, not yet
// sign-extended -- sign extension is the emitter's/effect's job, exactly
// like every other selected immediate operand).
[[nodiscard]] std::optional<M68kDecodeResult> m68k_decode_general_link_unlk(
    const DecodeSource &source, std::span<const std::uint8_t> image, std::size_t offset, std::uint64_t available,
    const std::array<std::uint8_t, 2> &bytes, std::uint16_t word) {
  const auto reg = static_cast<std::uint8_t>(word & 0x7U);
  if ((word & 0xFFF8U) == 0x4E58U) {
    const M68kEffectiveAddress an{M68kEaMode::address_register, reg, 0, 0, 0, 0};
    return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::unlk,
                                       M68kMemoryAccessWidth::long_word, {}, an, 0U);
  }
  if ((word & 0xFFF8U) == 0x4E50U) {
    if (available < 4U) {
      auto rejected = reject(DecodeOutcome::truncated_instruction, source, available, 4U, true);
      rejected.provenance = {source, bytes, ByteLength{2}};
      rejected.has_provenance = true;
      return rejected;
    }
    const auto ext_offset = offset + 2U;
    const auto disp16 = static_cast<std::uint16_t>((static_cast<std::uint16_t>(image[ext_offset]) << 8U) |
                                                    image[ext_offset + 1U]);
    const M68kEffectiveAddress an{M68kEaMode::address_register, reg, 0, 0, 0, 0};
    const M68kEffectiveAddress disp{M68kEaMode::immediate, 0, 0, 0, disp16, 1};
    return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::link,
                                       M68kMemoryAccessWidth::long_word, disp, an, 2U);
  }
  return std::nullopt;
}

// SEG-007-T025 (Batch C, C3): BTST/BCHG/BCLR/BSET. M68000PM/AD Rev. 1, §4,
// BTST/BCHG/BCLR/BSET entries; verified against the pinned Musashi
// m68k_in.c opcode table.
//
// Dynamic (Dn bit-number source) forms: bits15-12=0000 (fixed), bits11-9=
// source Dn (free), bits8-6=opmode (100/101/110/111 for BTST/BCHG/BCLR/
// BSET respectively, fixed), bits5-0=destination EA (free). Matched via
// mask 0xF1C0 (bits15-12 | bits8-6), leaving the register and EA fields
// free -- e.g. "0000...100......" for BTST.
//
// Static (immediate bit-number) forms: bits15-8=0x08 (fixed), bits7-6=
// selector (00/01/10/11 for BTST/BCHG/BCLR/BSET, fixed), bits5-0=
// destination EA (free) -- e.g. "0000100000......" for BTST. One 16-bit
// extension word (low byte significant) carries the raw bit number,
// decoded FIRST, exactly like ANDI/ORI/EORI's existing immediate-before-
// destination word order (m68k_decode_general_logical's `ordinary_immediate`
// above); no parallel immediate parser is introduced.
//
// Destination legality (contract: "exact project EA ceiling"): BCHG/BCLR/
// BSET modify their destination, so they reuse the existing
// `m68k_ea_data_alterable` set unchanged (Dn plus the six memory-alterable
// T023 forms; no An-direct, no PC-relative, no immediate -- PC-relative in
// particular must reject for all three because they write their
// destination). BTST is read-only and may additionally read via d16(PC)
// (`m68k_ea_bit_test_destination`), but per T025's deliberate scope
// decision, never an immediate destination even though the base ISA's
// dynamic BTST form architecturally permits one (Musashi's own legend:
// "A+-DXWLdxI"). Destination mode determines operation width: Dn -> 32-bit
// (long_word), any memory mode -> 8-bit (byte) -- there is no
// word-destination bit-operation form, so `size` is derived from the
// selected EA mode rather than any opcode size field (none exists).
//
// Load-bearing MOVEP collision (contract: "MOVEP collision is a load-bearing
// decoder test"): MOVEP's four forms ("0000rrr100001...", 101001, 110001,
// 111001 -- word/long, register<-memory and memory<-register) occupy the
// EXACT SAME `0000rrr1oo......` opmode slots (oo=00/01/10/11) as dynamic
// BTST/BCHG/BCLR/BSET, always at destination mode3==001 (address-register-
// direct in ordinary EA terms). mode3==001 is never a legal bit-operation
// destination on its own architectural merits -- it is absent from every
// legend row above (only mode3==000/Dn is ever listed as a register-shaped
// alternative). The shared EA legality check below therefore rejects every
// MOVEP encoding as an illegal-EA bit operation, through the existing
// unsupported-instruction route, with no MOVEP-specific exclusion or
// implementation required.
[[nodiscard]] std::optional<M68kDecodeResult> m68k_decode_general_bit_operation(
    const DecodeSource &source, std::span<const std::uint8_t> image, std::size_t offset, std::uint64_t available,
    const std::array<std::uint8_t, 2> &bytes, std::uint16_t word) {
  const auto mode3 = static_cast<std::uint8_t>((word >> 3U) & 0x7U);
  const auto reg3 = static_cast<std::uint8_t>(word & 0x7U);
  const auto finish = [&](M68kInstructionKind kind, const M68kEffectiveAddress &bit_number_ea,
                          std::uint32_t consumed_before_dst) -> std::optional<M68kDecodeResult> {
    // SEG-021-T008: full manual legality (see instruction.hpp); a dynamic BTST also admits #imm.
    const bool dynamic_bit_number = bit_number_ea.mode == M68kEaMode::data_register;
    const auto legal = kind != M68kInstructionKind::btst ? m68k_ea_bit_modify_destination
                       : dynamic_bit_number              ? m68k_ea_bit_test_dynamic_destination
                                                         : m68k_ea_bit_test_destination;
    // `size` here is only a placeholder for m68k_decode_one_ea's extension-byte
    // computation, which never depends on width for a non-immediate EA. The one
    // immediate destination -- the architecturally legal dynamic `BTST Dn,#<data>`
    // -- is a byte operand whose single extension word is read through the
    // byte-width placeholder, exactly like a byte immediate elsewhere; no other
    // bit operation can select an immediate destination. The real per-instruction
    // size is derived below from the EA mode actually selected.
    const auto dst = m68k_decode_one_ea(source, image, offset, available, bytes, consumed_before_dst, mode3, reg3,
                                        legal, M68kMemoryAccessWidth::byte);
    if (!dst.ok) return dst.failure;
    const auto size = dst.ea.mode == M68kEaMode::data_register ? M68kMemoryAccessWidth::long_word
                                                                : M68kMemoryAccessWidth::byte;
    return m68k_finish_general_decode(source, image, offset, bytes, kind, size, bit_number_ea, dst.ea,
                                      consumed_before_dst + dst.extension_bytes);
  };
  if ((word & 0xF1C0U) == 0x0100U || (word & 0xF1C0U) == 0x0140U || (word & 0xF1C0U) == 0x0180U ||
      (word & 0xF1C0U) == 0x01C0U) {
    const auto opmode = static_cast<std::uint8_t>((word >> 6U) & 0x7U);
    const auto source_reg = static_cast<std::uint8_t>((word >> 9U) & 0x7U);
    const M68kEffectiveAddress bit_source{M68kEaMode::data_register, source_reg, 0, 0, 0, 0};
    switch (opmode) {
    case 4U: return finish(M68kInstructionKind::btst, bit_source, 0U);
    case 5U: return finish(M68kInstructionKind::bchg, bit_source, 0U);
    case 6U: return finish(M68kInstructionKind::bclr, bit_source, 0U);
    case 7U: return finish(M68kInstructionKind::bset, bit_source, 0U);
    default: return std::nullopt;
    }
  }
  if ((word & 0xFF00U) == 0x0800U) {
    const auto selector = static_cast<std::uint8_t>((word >> 6U) & 0x3U);
    const auto imm = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, 7U, 4U, m68k_ea_immediate,
                                        M68kMemoryAccessWidth::word);
    if (!imm.ok) return imm.failure;
    switch (selector) {
    case 0U: return finish(M68kInstructionKind::btst, imm.ea, imm.extension_bytes);
    case 1U: return finish(M68kInstructionKind::bchg, imm.ea, imm.extension_bytes);
    case 2U: return finish(M68kInstructionKind::bclr, imm.ea, imm.extension_bytes);
    default: return finish(M68kInstructionKind::bset, imm.ea, imm.extension_bytes);
    }
  }
  return std::nullopt;
}

// SEG-007-T025 (Batch C, C4): the shared 4-bit Bcc/DBcc condition-field
// selector (contract: "one shared condition-code owner"). 0000/0001 are
// BRA/BSR's and DBT/DBF's own slots, never constructed here; Bcc/DBcc's own
// decoders only ever pass 2-15.
[[nodiscard]] M68kCondition m68k_condition_from_selector(std::uint8_t selector) noexcept {
  switch (selector) {
  case 2U: return M68kCondition::hi;
  case 3U: return M68kCondition::ls;
  case 4U: return M68kCondition::cc;
  case 5U: return M68kCondition::cs;
  case 6U: return M68kCondition::ne;
  case 7U: return M68kCondition::eq;
  case 8U: return M68kCondition::vc;
  case 9U: return M68kCondition::vs;
  case 10U: return M68kCondition::pl;
  case 11U: return M68kCondition::mi;
  case 12U: return M68kCondition::ge;
  case 13U: return M68kCondition::lt;
  case 14U: return M68kCondition::gt;
  default: return M68kCondition::le;  // 15
  }
}

// SEG-007-T025 (Batch C, C4a): BRA and every selected Bcc condition,
// generalizing both into one shared decode route rather than one kind per
// condition (contract: "generic branch identity"). Primary word shape
// "0110 cccc dddddddd" (M68000PM/AD Rev. 1 §4; verified against pinned
// Musashi's bra/bcc opcode table). cccc==0000 selects BRA (condition
// `always`); cccc==0001 selects BSR, which this decoder deliberately does
// NOT claim (a later C4 checkpoint on this same task branch owns it; see
// m68k_decode_general_bsr below/BSR's own comment once added).
//
// Base MC68000 displacement selection (contract: "verify MC68000 branch
// encodings"): low byte == 0x00 selects a signed 16-bit extension-word
// displacement; ANY other low byte -- including 0xFF -- is the ordinary
// signed 8-bit displacement already in the primary word. This function
// deliberately never special-cases 0xFF: the plain byte-form branch below
// already covers it naturally, exactly matching pinned Musashi's bra/bcc/
// bsr "32" opcode variants, each of which is gated behind
// `CPU_TYPE_IS_EC020_PLUS` and falls back to the identical plain 8-bit form
// otherwise -- so a base-MC68000 CPU type never consumes a long-branch
// extension at all. The word-form's own `available < 4` truncation check
// below is therefore the ONLY way a low byte of 0xFF could ever fail to
// select the two-byte primary alone, and 0xFF never reaches it (since
// `bytes[1] != 0` is already true for 0xFF).
[[nodiscard]] std::optional<M68kDecodeResult> m68k_decode_general_branch(
    const DecodeSource &source, std::span<const std::uint8_t> image, std::size_t offset, std::uint64_t available,
    const std::array<std::uint8_t, 2> &bytes, std::uint16_t word) {
  if ((word & 0xF000U) != 0x6000U) return std::nullopt;
  const auto selector = static_cast<std::uint8_t>((word >> 8U) & 0xFU);
  if (selector == 1U) return std::nullopt;  // BSR: not this decoder's job (see doc comment)
  const auto condition = selector == 0U ? M68kCondition::always : m68k_condition_from_selector(selector);
  if (bytes[1] != 0U) {
    const M68kEffectiveAddress disp{M68kEaMode::immediate, 0, 0, 0, bytes[1], 0};
    auto instruction = m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::branch,
                                                   M68kMemoryAccessWidth::byte, disp, {}, 0U);
    instruction.condition = condition;
    return instruction;
  }
  if (available < 4U) {
    auto rejected = reject(DecodeOutcome::truncated_instruction, source, available, 4U, true);
    rejected.provenance = {source, bytes, ByteLength{2}};
    rejected.has_provenance = true;
    return rejected;
  }
  const auto ext_offset = offset + 2U;
  const auto disp16 = static_cast<std::uint16_t>((static_cast<std::uint16_t>(image[ext_offset]) << 8U) |
                                                  image[ext_offset + 1U]);
  const M68kEffectiveAddress disp{M68kEaMode::immediate, 0, 0, 0, disp16, 1};
  auto instruction = m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::branch,
                                                 M68kMemoryAccessWidth::word, disp, {}, 2U);
  instruction.condition = condition;
  return instruction;
}

// SEG-007-T025 (Batch C, C4b): BSR, occupying the condition-field slot
// (cccc==0001) m68k_decode_general_branch above deliberately does not
// claim. BSR is a direct call, not a plain branch (contract: "BSR is a
// call, not just BRA with a different nibble"): it shares this decoder's
// primary-word structure and displacement-selection rule (low byte == 0x00
// selects a signed 16-bit extension displacement; any other low byte,
// including 0xFF, is the ordinary signed 8-bit displacement -- verified
// against pinned Musashi's bsr opcode table, whose own "32" 0xFF-shaped
// variant is likewise gated behind CPU_TYPE_IS_EC020_PLUS and otherwise
// falls back to the plain 8-bit form) but is decoded as its own truthful
// M68kInstructionKind::bsr identity, never a fake M68kInstructionKind::jsr
// or M68kInstructionKind::branch.
[[nodiscard]] std::optional<M68kDecodeResult> m68k_decode_general_bsr(
    const DecodeSource &source, std::span<const std::uint8_t> image, std::size_t offset, std::uint64_t available,
    const std::array<std::uint8_t, 2> &bytes, std::uint16_t word) {
  if ((word & 0xFF00U) != 0x6100U) return std::nullopt;
  if (bytes[1] != 0U) {
    const M68kEffectiveAddress disp{M68kEaMode::immediate, 0, 0, 0, bytes[1], 0};
    return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::bsr,
                                       M68kMemoryAccessWidth::byte, disp, {}, 0U);
  }
  if (available < 4U) {
    auto rejected = reject(DecodeOutcome::truncated_instruction, source, available, 4U, true);
    rejected.provenance = {source, bytes, ByteLength{2}};
    rejected.has_provenance = true;
    return rejected;
  }
  const auto ext_offset = offset + 2U;
  const auto disp16 = static_cast<std::uint16_t>((static_cast<std::uint16_t>(image[ext_offset]) << 8U) |
                                                  image[ext_offset + 1U]);
  const M68kEffectiveAddress disp{M68kEaMode::immediate, 0, 0, 0, disp16, 1};
  return m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::bsr,
                                     M68kMemoryAccessWidth::word, disp, {}, 2U);
}

// SEG-007-T025 (Batch C, C4c): DBcc Dn,<word displacement>. Primary word
// shape "0101 cccc 11001 rrr" (M68000PM/AD Rev. 1 §4; verified against
// pinned Musashi's dbcc/dbf/dbt opcode table -- e.g. cccc=0001/DBF/DBRA
// gives the well-known 0x51C8|reg base). `cccc` selects the shared
// condition exactly like Bcc, but DBT (0000) and DBF/DBRA (0001) ARE
// selected here (unlike Bcc, which never constructs `always`/`never` --
// those bit patterns are BRA/BSR instead).
//
// Load-bearing Scc collision (contract: "DBcc exact encoding and Scc
// collision"): DBcc's fixed bits7-3 ("11001") occupy exactly the
// mode3==001 (An-direct) destination slot within the broader Scc family's
// own "0101 cccc 11 " + 6-bit-EA shape -- a slot that is never a legal Scc
// destination on its own architectural merits (Scc's destination is Dn or
// one of the six memory-alterable modes, never An-direct). The mask below
// (0xF0F8, matching bits15-12 and bits7-3 exactly) is therefore a precise,
// narrow DBcc-only selector that cannot widen into any legal Scc encoding;
// this project does not implement Scc at all.
[[nodiscard]] std::optional<M68kDecodeResult> m68k_decode_general_dbcc(
    const DecodeSource &source, std::span<const std::uint8_t> image, std::size_t offset, std::uint64_t available,
    const std::array<std::uint8_t, 2> &bytes, std::uint16_t word) {
  if ((word & 0xF0F8U) != 0x50C8U) return std::nullopt;
  const auto selector = static_cast<std::uint8_t>((word >> 8U) & 0xFU);
  const auto reg = static_cast<std::uint8_t>(word & 0x7U);
  const auto condition = selector == 0U   ? M68kCondition::always
                        : selector == 1U ? M68kCondition::never
                                          : m68k_condition_from_selector(selector);
  if (available < 4U) {
    auto rejected = reject(DecodeOutcome::truncated_instruction, source, available, 4U, true);
    rejected.provenance = {source, bytes, ByteLength{2}};
    rejected.has_provenance = true;
    return rejected;
  }
  const auto ext_offset = offset + 2U;
  const auto disp16 = static_cast<std::uint16_t>((static_cast<std::uint16_t>(image[ext_offset]) << 8U) |
                                                  image[ext_offset + 1U]);
  const M68kEffectiveAddress disp{M68kEaMode::immediate, 0, 0, 0, disp16, 1};
  const M68kEffectiveAddress dn{M68kEaMode::data_register, reg, 0, 0, 0, 0};
  auto instruction = m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::dbcc,
                                                 M68kMemoryAccessWidth::word, disp, dn, 2U);
  instruction.condition = condition;
  return instruction;
}

// SEG-007-T025 (Batch C, C5a): MOVEM.W/L, register-list<->memory ordinary
// (non-auto-update) forms only; -(An) (C5b) and (An)+ (C5c) are added by
// their own later checkpoints on this same task branch, never here.
// M68000PM/AD Rev. 1 Sec 4, MOVEM entry; verified against pinned Musashi's
// m68k_in.c opcode table ("re ."/"er ." rows, legend "A..DXWL..."; the
// dedicated "er pcdi" row additionally selects d16(PC) for the
// memory->register direction only -- see the header's
// m68k_ea_movem_register_to_memory/m68k_ea_movem_memory_to_register doc
// comment for the confirmed exact EA matrix).
//
// Primary word fixed bits (mask 0xFB80, matching every direction(bit10)/
// size(bit6)/EA(bits5-0) combination): bits15-12 = 0100, bit11 = 1, bits9-8
// = 00, bit7 = 1. bit10 = direction (0 = registers_to_memory, 1 =
// memory_to_registers); bit6 = size (0 = word, 1 = long); bits5-0 = the
// ordinary EA field (mode3/reg3), exactly like every other selected EA-
// bearing form.
//
// Opcode/extension order (contract: "opcode/extension order is load-
// bearing"): the 16-bit register-mask extension word is ALWAYS present and
// always comes BEFORE the EA's own extension word(s) -- verified against
// pinned Musashi's OPER_I_16() (mask) preceding M68KMAKE_GET_EA_AY_16/32
// (EA) in every selected handler. m68k_decode_one_ea's `consumed_before`
// parameter is passed 2 (the mask word) so the shared EA parser can never
// mistake the mask for its own extension, matching MOVE's existing source-
// before-destination `consumed_before` convention.
//
// No mode3==000/001 (Dn/An-direct) collision risk: MOVEM's legal EA sets
// never include Dn or An-direct (absent from the pinned legend), so the
// shared EA legality check rejects them through the ordinary unsupported-
// instruction route; in particular mode3==000 (word register->memory,
// 0x4880-0x4887) is already exhaustively claimed by EXT.W's exact decoder
// (m68k_decode_general_swap_ext), which the dispatch chain below places
// before this decoder, exactly like SWAP-before-PEA.
[[nodiscard]] std::optional<M68kDecodeResult> m68k_decode_general_movem(
    const DecodeSource &source, std::span<const std::uint8_t> image, std::size_t offset, std::uint64_t available,
    const std::array<std::uint8_t, 2> &bytes, std::uint16_t word) {
  if ((word & 0xFB80U) != 0x4880U) return std::nullopt;
  const auto direction = ((word >> 10U) & 1U) != 0U ? M68kMovemDirection::memory_to_registers
                                                     : M68kMovemDirection::registers_to_memory;
  const auto size = ((word >> 6U) & 1U) != 0U ? M68kMemoryAccessWidth::long_word : M68kMemoryAccessWidth::word;
  const auto mode3 = static_cast<std::uint8_t>((word >> 3U) & 0x7U);
  const auto reg3 = static_cast<std::uint8_t>(word & 0x7U);
  if (available < 4U) {
    auto rejected = reject(DecodeOutcome::truncated_instruction, source, available, 4U, true);
    rejected.provenance = {source, bytes, ByteLength{2}};
    rejected.has_provenance = true;
    return rejected;
  }
  const auto mask_offset = offset + 2U;
  const auto register_mask = static_cast<std::uint16_t>((static_cast<std::uint16_t>(image[mask_offset]) << 8U) |
                                                         image[mask_offset + 1U]);
  const auto legal = direction == M68kMovemDirection::registers_to_memory ? m68k_ea_movem_register_to_memory
                                                                          : m68k_ea_movem_memory_to_register;
  const auto ea = m68k_decode_one_ea(source, image, offset, available, bytes, 2U, mode3, reg3, legal, size);
  if (!ea.ok) return ea.failure;
  const M68kEffectiveAddress unused{};
  auto instruction = direction == M68kMovemDirection::registers_to_memory
      ? m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::movem, size, unused, ea.ea,
                                    2U + ea.extension_bytes)
      : m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::movem, size, ea.ea, unused,
                                    2U + ea.extension_bytes);
  instruction.movem_direction = direction;
  instruction.movem_register_mask = register_mask;
  return instruction;
}

// SEG-007-T025 (Batch C, C6): register-form shift/rotate. M68000PM/AD Rev.
// 1 Sec 4, ASL/ASR/LSL/LSR/ROL/ROR/ROXL/ROXR entries; verified bit-for-bit
// against pinned Musashi's m68k_in.c opcode table (its "s"/"r" rows for
// every one of the 8 families across all three sizes) before writing this
// decoder (contract: "verify register-shift encoding before coding").
//
// Primary word layout: `1110 ccc d ss i ff rrr` (bits15-12 fixed 1110;
// bits11-9 `ccc` = immediate count (1-7) or count-source register number
// (0-7); bit8 `d` = direction, 0=right/1=left; bits7-6 `ss` = size
// (00=byte,01=word,10=long -- `ss==11` NEVER appears in any selected
// register-form row); bit5 `i` = count source, 0=immediate/1=register;
// bits4-3 `ff` = family selector (00=AS,01=LS,10=ROX,11=RO); bits2-0 `rrr`
// = destination Dn).
//
// Load-bearing C6/C7 decode collision (contract: "load-bearing C6/C7
// decode collision"): the memory-WORD forms sharing this same 0xE000
// primary-word family always fix `ss` (bits7-6) to `11` (verified against
// every one of the 8 families' own "." memory-form row, e.g. ASR's
// `1110000011......`), a combination NEVER produced by any register-form
// row above. Rejecting `ss==11` outright therefore excludes EVERY memory
// form precisely and completely -- not merely by decoder dispatch order --
// leaving that entire encoding space exclusively for C7, which this
// checkpoint never touches.
//
// C6a selected the LS family (`ff==01`); C6b widened the same family switch
// (never a second decoder) to additionally select the AS family
// (`ff==00`); C6c further widened it to select the RO family (`ff==11`,
// direction distinguishing ROR/ROL); C6d completes it by selecting the ROX
// family (`ff==10`, direction distinguishing ROXR/ROXL). All 8 base-
// MC68000 register-form families are now selected by this ONE decoder --
// no second ROX-specific decoder was introduced. The memory-form exclusion
// (`ss==11`, checked above) remains the sole boundary reserving C7's
// encoding space; nothing else falls through to the unsupported-
// instruction-form rejection for this primary opcode family anymore.
[[nodiscard]] std::optional<M68kDecodeResult> m68k_decode_general_shift_rotate(
    const DecodeSource &source, std::span<const std::uint8_t> image, std::size_t offset,
    const std::array<std::uint8_t, 2> &bytes, std::uint16_t word) {
  if ((word & 0xF000U) != 0xE000U) return std::nullopt;
  if ((word & 0x00C0U) == 0x00C0U) return std::nullopt;  // memory form (C7) or otherwise not ours
  const auto count_or_reg = static_cast<std::uint8_t>((word >> 9U) & 0x7U);
  const auto direction_left = ((word >> 8U) & 0x1U) != 0U;
  const auto size_field = static_cast<std::uint8_t>((word >> 6U) & 0x3U);
  const auto register_count = ((word >> 5U) & 0x1U) != 0U;
  const auto family = static_cast<std::uint8_t>((word >> 3U) & 0x3U);
  const auto dest_reg = static_cast<std::uint8_t>(word & 0x7U);
  M68kShiftRotateKind kind{};
  switch (family) {
  case 0U: kind = direction_left ? M68kShiftRotateKind::asl : M68kShiftRotateKind::asr; break;
  case 1U: kind = direction_left ? M68kShiftRotateKind::lsl : M68kShiftRotateKind::lsr; break;
  case 2U: kind = direction_left ? M68kShiftRotateKind::roxl : M68kShiftRotateKind::roxr; break;
  case 3U: kind = direction_left ? M68kShiftRotateKind::rol : M68kShiftRotateKind::ror; break;
  default: return std::nullopt;  // unreachable: `family` is a 2-bit field (0..3), all 4 selected
  }
  const auto size = size_field == 0U ? M68kMemoryAccessWidth::byte
                   : size_field == 1U ? M68kMemoryAccessWidth::word : M68kMemoryAccessWidth::long_word;
  const M68kEffectiveAddress dest_ea{M68kEaMode::data_register, dest_reg, 0, 0, 0, 0};
  M68kEffectiveAddress count_ea{};
  if (register_count) {
    count_ea = {M68kEaMode::data_register, count_or_reg, 0, 0, 0, 0};
  } else {
    // The encoded 3-bit immediate count field's 0->8 mapping (contract:
    // "immediate count") is a pure, register-independent bit computation,
    // resolved exactly once here -- never re-interpreted by any later
    // stage.
    const std::uint32_t resolved_count = count_or_reg == 0U ? 8U : count_or_reg;
    count_ea = {M68kEaMode::immediate, 0, 0, 0, resolved_count, 0};
  }
  auto instruction = m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::shift_rotate,
                                                 size, count_ea, dest_ea, 0U);
  instruction.shift_rotate_kind = kind;
  return instruction;
}

// SEG-007-T025 (Batch C, C7a): the memory-WORD shift/rotate forms C6
// deliberately excluded via its own `ss==11` check -- structurally disjoint
// from the register decoder above, which requires `ss != 11`; this decoder
// requires it. Verified directly against pinned Musashi's disassembler
// opcode table (`m68kdasm.c`'s `g_opcode_info` mask/base pairs): mask
// 0xFFC0, base 0xE0C0/0xE1C0/0xE2C0/0xE3C0/0xE4C0/0xE5C0/0xE6C0/0xE7C0 for
// ASR/ASL/LSR/LSL/ROXR/ROXL/ROR/ROL respectively. Generalizing those 8
// literal bases: bits15-12 fixed `1110` (shared with every C6 word), bits11-9
// select the family (`000`=AS,`001`=LS,`010`=ROX,`011`=RO -- the SAME
// numbering the register decoder's `ff` field uses, just relocated to a
// different bit position for this word shape), bit8 selects direction
// (0=right,1=left, same convention as the register decoder), bits7-6 fixed
// to `11` (the shared C6/C7 structural boundary), bits5-0 are the EA. Family
// values 4-7 (bits11-9 `100`-`111`) are 68020+ bitfield instructions
// (BFTST/BFCHG/BFCLR/BFEXTU/... starting at 0xE8C0, confirmed against the
// same pinned disassembler table) -- permanently out of this project's
// MC68000-era scope, correctly rejected by the same `default` case that
// used to also reject ROX/RO pending C7b. C7a selected only the AS/LS
// families (`ASR`/`ASL`/`LSR`/`LSL`); C7b widens this SAME family switch
// (never a second memory decoder) to add ROX/RO (`ROXR`/`ROXL`/`ROR`/`ROL`),
// exactly mirroring how C6b/C6c/C6d progressively widened the register
// decoder's own family switch -- completing the final 8-family memory
// matrix. The count is architecturally fixed to exactly 1 -- there is no
// count field in this word shape at all, unlike every register form.
[[nodiscard]] std::optional<M68kDecodeResult> m68k_decode_general_shift_rotate_memory(
    const DecodeSource &source, std::span<const std::uint8_t> image, std::size_t offset, std::uint64_t available,
    const std::array<std::uint8_t, 2> &bytes, std::uint16_t word) {
  if ((word & 0xF0C0U) != 0xE0C0U) return std::nullopt;
  const auto family = static_cast<std::uint8_t>((word >> 9U) & 0x7U);
  const auto direction_left = ((word >> 8U) & 0x1U) != 0U;
  M68kShiftRotateKind kind{};
  switch (family) {
  case 0U: kind = direction_left ? M68kShiftRotateKind::asl : M68kShiftRotateKind::asr; break;
  case 1U: kind = direction_left ? M68kShiftRotateKind::lsl : M68kShiftRotateKind::lsr; break;
  case 2U: kind = direction_left ? M68kShiftRotateKind::roxl : M68kShiftRotateKind::roxr; break;
  case 3U: kind = direction_left ? M68kShiftRotateKind::rol : M68kShiftRotateKind::ror; break;
  default:
    return std::nullopt;
    // family 4-7 is reachable 68020+ bitfield space (BFTST/BFCHG/...
    // starting at 0xE8C0), deliberately rejected -- outside the selected
    // base-MC68000 scope, not an unreachable case.
  }
  const auto mode3 = static_cast<std::uint8_t>((word >> 3U) & 0x7U);
  const auto reg3 = static_cast<std::uint8_t>(word & 0x7U);
  // Contract "exact base-MC68000 C7 matrix": legal EA is exactly
  // `m68k_ea_shift_memory_destination` (SEG-021-T009) -- (An)/(An)+/-(An)/d16(An)/(d8,An,Xn)/absolute.w/
  // absolute.l, deliberately excluding Dn/An-direct/PC-relative/immediate. Size is always WORD; there is no BYTE or LONG memory form.
  const auto dst = m68k_decode_one_ea(source, image, offset, available, bytes, 0U, mode3, reg3,
                                       m68k_ea_shift_memory_destination, M68kMemoryAccessWidth::word);
  if (!dst.ok) return dst.failure;
  // `source_ea` is deliberately left default/unused: the memory form has no
  // decoded count operand at all (always exactly 1), unlike every register
  // form's `source_ea`-carried count fact.
  auto instruction = m68k_finish_general_decode(source, image, offset, bytes, M68kInstructionKind::shift_rotate,
                                                 M68kMemoryAccessWidth::word, {}, dst.ea, dst.extension_bytes);
  instruction.shift_rotate_kind = kind;
  return instruction;
}
} // namespace

M68kDecodeResult decode_m68k_instruction(std::span<const std::uint8_t> image, DecodeSource source,
                                          M68kDecodeProfile profile) {
  const auto image_size = static_cast<std::uint64_t>(image.size());
  const auto available = source.image_offset.value > image_size ? 0U : image_size - source.image_offset.value;
  if (source.cpu_variant != CpuVariant::mc68000)
    return reject(DecodeOutcome::unsupported_cpu_variant, source, available, 2U, false);
  if (source.address.space != TargetAddressSpace::m68k_program)
    return reject(DecodeOutcome::unsupported_address_space, source, available, 2U, false);
  if ((source.address.value & 1U) != 0U)
    return reject(DecodeOutcome::odd_instruction_address, source, available, 2U, false);
  if (available < 2U) return reject(DecodeOutcome::truncated_instruction, source, available, 2U, false);

  const auto offset = static_cast<std::size_t>(source.image_offset.value);
  const std::array<std::uint8_t, 2> bytes{image[offset], image[offset + 1U]};
  const auto word = static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8U) | bytes[1]);
  const auto reject_word = [&](DecodeOutcome outcome, std::uint32_t requested) {
    auto result = reject(outcome, source, available, requested, true);
    result.provenance = {source, bytes, ByteLength{2}};
    result.has_provenance = true;
    return result;
  };
  // ILLEGAL is a source-word property, not a property of the selected slice.
  // Check it before profile-specific supported-form selection so every route
  // retains the inherited source-decode precedence.
  if (word == 0x4AFCU)
    return reject_word(DecodeOutcome::illegal_instruction, 2U);

  M68kDecodedInstruction instruction{};
  instruction.provenance = {source, bytes, ByteLength{2}};
  instruction.destination = static_cast<DataRegister>((word >> 9U) & 0x7U);
  instruction.operand = static_cast<std::int8_t>(bytes[1]);
  if (profile == M68kDecodeProfile::genesis_startup || profile == M68kDecodeProfile::general_startup) {
    const auto select = [&](M68kInstructionKind kind, std::uint32_t length) -> M68kDecodeResult {
      if (available < length) return reject_word(DecodeOutcome::truncated_instruction, length);
      instruction.kind = kind;
      instruction.provenance.length = ByteLength{length};
      // This is the single verified read of the complete selected span; no
      // later stage may independently re-read the source image for bytes
      // already verified by this bounds check.
      instruction.raw_bytes.assign(image.begin() + static_cast<std::ptrdiff_t>(offset),
                                   image.begin() + static_cast<std::ptrdiff_t>(offset + length));
      if (length == 6U)
        instruction.extension = (static_cast<std::uint32_t>(instruction.raw_bytes[2]) << 24U) |
                                (static_cast<std::uint32_t>(instruction.raw_bytes[3]) << 16U) |
                                (static_cast<std::uint32_t>(instruction.raw_bytes[4]) << 8U) |
                                instruction.raw_bytes[5];
      return instruction;
    };
    if ((word & 0xFF00U) == 0x7000U) return select(M68kInstructionKind::moveq, 2U);
    // SEG-007-T086: general_startup's own MOVEQ recognition (the check
    // immediately above this one, shared with genesis_startup) only ever
    // matched destination register D0 (mask 0xFF00 forces the Dn field to
    // zero). MOVEQ's encoding/operation is register-independent
    // (moveq-contract.md, m68k-common-startup-data-movement-batch-contract.md),
    // and its lift (M68kInstructionKind::moveq) and C4 emission
    // (M68kIrKind::write_moveq, which reads operation.destination directly)
    // are already fully generic over D0-D7; only decode-time recognition was
    // narrower than the semantics it already selects. Widen general_startup
    // only (genesis_startup's own D0-only behavior above is unchanged) to
    // accept any destination register, reusing the same select() helper so
    // raw_bytes/length/extension are populated identically to the D0 case.
    if (profile == M68kDecodeProfile::general_startup && (word & 0xF100U) == 0x7000U)
      return select(M68kInstructionKind::moveq, 2U);
    // Fixed-startup reporting adapts shared records, so exact absolute forms
    // continue through the same MOVE/JSR EA decoder as every selected form.
    if (word == 0x4E75U) return select(M68kInstructionKind::rts, 2U);
    // SEG-007-T047 / ADR-0020 §9: RTE (0x4E73), general_startup policy only.
    // genesis_startup keeps rejecting it as before.
    if (profile == M68kDecodeProfile::general_startup && word == 0x4E73U)
      return select(M68kInstructionKind::rte, 2U);
    // These forms remain deliberately unselected.  They are nevertheless
    // recognized here so general-startup can distinguish a verified complete
    // CPU frontier from an arbitrary rejected primary word. The full span is
    // bounds-checked before rejection; STOP with no extension remains a
    // truncation, not a frontier. The public rejected provenance/fetch fields
    // are the sole representation of this fact.
    // MOVE An,USP (0x4E60-0x4E67 inclusive) is recognized here alongside
    // RESET/NOP/STOP for the identical reason: general-startup must
    // distinguish this verified complete CPU frontier from an arbitrary
    // rejected primary word. The reverse direction MOVE USP,An
    // (0x4E68-0x4E6F) and every other neighboring System Control Group
    // encoding are deliberately excluded and remain unrecognized.
    if (profile == M68kDecodeProfile::general_startup && word >= 0x4E60U && word <= 0x4E67U) {
      auto result = select(M68kInstructionKind::move_an_to_usp, 2U);
      auto &selected = std::get<M68kDecodedInstruction>(result);
      selected.source_ea = {M68kEaMode::address_register,
                            static_cast<std::uint8_t>(word & UINT16_C(0x0007))};
      return result;
    }
    // SEG-007-T114: NOP (0x4E71) is now a selected general_startup capability,
    // decoded through the same shared select() helper as every other selected
    // form so raw_bytes/length are populated identically. Per the public
    // Motorola M68000 Family PRM NOP entry, the only architectural effect is
    // the two-byte PC advance -- no operand, no CCR/SR effect, no memory or
    // device access, unprivileged (see docs/references/m68k-nop-contract.md).
    // select() performs the full `available >= 2` bounds check and rejects a
    // truncated 0x4E71 as truncated_instruction. genesis_startup's own
    // behavior for 0x4E71 is unchanged: this branch and the CPU-frontier
    // rejection cluster below are both general_startup-only, so genesis_startup
    // still falls through to its own unconditional
    // valid_but_unsupported_instruction rejection later in this function.
    // RESET (0x4E70) and STOP (0x4E72) remain in the reject cluster below
    // exactly as before -- each needs architecture this task must not add
    // (external-device reset, privilege + halt/interrupt-wait).
    if (profile == M68kDecodeProfile::general_startup && word == 0x4E71U)
      return select(M68kInstructionKind::nop, 2U);
    if (profile == M68kDecodeProfile::general_startup &&
        (word == 0x4E70U || word == 0x4E72U)) {
      const auto length = word == 0x4E72U ? 4U : 2U;
      if (available < length) return reject_word(DecodeOutcome::truncated_instruction, length);
      auto result = reject_word(DecodeOutcome::valid_but_unsupported_instruction, length);
      result.instruction_length = length;
      result.provenance.length = ByteLength{length};
      result.cpu_frontier = classify_m68k_cpu_frontier(result.provenance);
      return result;
    }
    // TST.L (xxx).L (SEG-007-T004/T008): opcode byte 0x4A, size field long
    // (top two bits of the low byte == 0b10), excluding the reserved
    // TAS/ILLEGAL size-field value 0b11 (0xC0 in the low byte). This is the
    // sole shared TST decode predicate/selection point in the whole
    // pipeline: a genesis_startup CPU-capability fact, independent of
    // whether the fixed genesis_rom_startup graph (analyze_startup_profile)
    // ever accepts a TST.L at any of its fixed role positions.
    if ((word & 0xFF00U) == 0x4A00U && (word & 0xC0U) != 0xC0U) {
       if (word == 0x4AB9U)
         return m68k_decode_general_tst(source, image, offset, available, bytes, word);
      // SEG-007-T023: every other TST.B/W/L size/addressing-mode combination
      // is a genuinely new general_startup-only capability, decoded through
      // the one shared EA layer; genesis_startup's own decode behavior here
      // is completely unchanged (still an unconditional
      // unsupported_instruction_form for anything but the exact absolute-long
      // word above).
      if (profile == M68kDecodeProfile::general_startup)
        return m68k_decode_general_tst(source, image, offset, available, bytes, word);
      // Same selected opcode family, wrong form (e.g. TST.L D0, or any TST
      // size/addressing mode other than long/absolute-long).
      auto result = reject_word(DecodeOutcome::valid_but_unsupported_instruction, 2U);
      result.unsupported_instruction_form = true;
      return result;
    }
    // SEG-007-T023: MOVE/MOVEA/CLR/LEA/JMP/JSR's general whitelist forms are
    // general_startup-only new capability, decoded through the same shared
    // EA layer; genesis_startup's own decode behavior is entirely untouched
    // by this block (it never reaches it).
     if (profile == M68kDecodeProfile::general_startup || word == 0x23C0U || word == 0x2239U || word == 0x4EB9U) {
      if (auto general = m68k_decode_general_move(source, image, offset, available, bytes, word)) return *general;
        if (auto general = m68k_decode_general_compare(source, image, offset, available, bytes, word)) return *general;
        if (auto general = m68k_decode_general_add(source, image, offset, available, bytes, word)) return *general;
         if (auto general = m68k_decode_general_subtract(source, image, offset, available, bytes, word)) return *general;
         if (auto general = m68k_decode_general_logical(source, image, offset, available, bytes, word)) return *general;
      if (auto general = m68k_decode_general_clr(source, image, offset, available, bytes, word)) return *general;
      if (auto general = m68k_decode_general_not(source, image, offset, available, bytes, word)) return *general;
       if (auto general = m68k_decode_general_negate_word(source, image, offset, available, bytes, word)) return *general;
      if (auto general = m68k_decode_general_move_to_sr(source, image, offset, available, bytes, word)) return *general;
      if (auto general = m68k_decode_general_move_from_sr(source, image, offset, available, bytes, word)) return *general;
      if (auto general = m68k_decode_general_move_to_ccr(source, image, offset, available, bytes, word)) return *general;
      // m68k_decode_general_swap_ext MUST run before m68k_decode_general_pea:
      // SWAP's exact-Dn mask exhaustively claims 0x4840-0x4847 first, so PEA's
      // broader 0xFFC0 mask over the rest of 0x4840-0x487F never contends
      // with it (see m68k_decode_general_pea's doc comment).
      if (auto general = m68k_decode_general_swap_ext(source, image, offset, bytes, word)) return *general;
      if (auto general = m68k_decode_general_pea(source, image, offset, available, bytes, word)) return *general;
      if (auto general = m68k_decode_general_link_unlk(source, image, offset, available, bytes, word)) return *general;
      if (auto general = m68k_decode_general_bit_operation(source, image, offset, available, bytes, word)) return *general;
      if (auto general = m68k_decode_general_branch(source, image, offset, available, bytes, word)) return *general;
      if (auto general = m68k_decode_general_bsr(source, image, offset, available, bytes, word)) return *general;
      if (auto general = m68k_decode_general_dbcc(source, image, offset, available, bytes, word)) return *general;
      if (auto general = m68k_decode_general_movem(source, image, offset, available, bytes, word)) return *general;
      if (auto general = m68k_decode_general_shift_rotate(source, image, offset, bytes, word)) return *general;
      if (auto general = m68k_decode_general_shift_rotate_memory(source, image, offset, available, bytes, word))
        return *general;
      if (auto general = m68k_decode_general_lea(source, image, offset, available, bytes, word)) return *general;
      if (auto general = m68k_decode_general_jmp(source, image, offset, available, bytes, word)) return *general;
       if (auto general = m68k_decode_general_jsr(source, image, offset, available, bytes, word)) return *general;
    }
    // general_startup (SEG-007-T010) additionally selects direct_flow's own
    // forms (SUBQ.L #1,D0/BNE.short/BRA.short) through the exact same shared
    // predicates below; only genesis_startup keeps its original unconditional
    // "nothing in this family matched" rejection here.
    if (profile == M68kDecodeProfile::genesis_startup) {
      auto result = reject_word(DecodeOutcome::valid_but_unsupported_instruction, 2U);
      result.unsupported_instruction_form = (word & 0xFFC0U) == 0x4E80U ||
                                            (word & 0xF000U) == 0x2000U ||
                                            (word & 0xF100U) == 0x7000U;
      return result;
    }
  }
  if ((word & 0xF100U) == 0x7000U &&
      (profile == M68kDecodeProfile::moveq || instruction.destination == DataRegister::d0)) {
    instruction.kind = M68kInstructionKind::moveq;
    return instruction;
  }
  if (profile == M68kDecodeProfile::direct_flow &&
      word == 0x5380U) {
    instruction.kind = M68kInstructionKind::subq_l_1_d0;
    instruction.destination = DataRegister::d0;
    instruction.operand = 0;
    return instruction;
  }
  // SEG-007-T025 (Batch C, C4a): general_startup no longer reaches this
  // narrow BNE/BRA-only compatibility pair -- its own general branch
  // decoder above (m68k_decode_general_branch, inside the general_startup
  // block) already claims the entire 0110-prefix family first and returns.
  // This pair remains the sole selected branch form for the frozen
  // direct_flow profile only (contract: "preserve old direct-flow
  // compatibility"); DirectFlowKind::bne_short/bra_short and their existing
  // tests are unchanged.
  if (profile == M68kDecodeProfile::direct_flow &&
      (word & 0xFF00U) == 0x6600U && bytes[1] != 0U) {
    instruction.kind = M68kInstructionKind::bne_short;
    return instruction;
  }
  if (profile == M68kDecodeProfile::direct_flow &&
      (word & 0xFF00U) == 0x6000U && bytes[1] != 0U) {
    instruction.kind = M68kInstructionKind::bra_short;
    return instruction;
  }
  if (profile == M68kDecodeProfile::direct_flow &&
      ((word & 0xFF00U) == 0x6600U || (word & 0xFF00U) == 0x6000U)) {
    if (available < 4U) return reject_word(DecodeOutcome::truncated_instruction, 4U);
    return reject_word(DecodeOutcome::valid_but_unsupported_instruction, 4U);
  }
  return reject_word(DecodeOutcome::valid_but_unsupported_instruction, 2U);
}

} // namespace segarecomp
