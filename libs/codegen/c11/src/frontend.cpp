#include "segarecomp/codegen/c11/compiled_entry_table.hpp"
#include "segarecomp/codegen/c11/genesis_frontend.hpp"
#include "segarecomp/codegen/c11/genesis.hpp"
#include "segarecomp/codegen/c11/translation_units.hpp"
#include "segarecomp/cpu/m68k/timing.hpp"
#include <algorithm>
#include <cstdio>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>

namespace segarecomp {
namespace {
// SEG-022-T003: the shared-header helper functions become `static inline` in a sharded build.
// SEG-022-T008: bounded host-owner size (entries per generated AOT owner function).
constexpr std::size_t aot_owner_max_entries = 128U;
// SEG-022-T011: generated routed-failure tail (see its definition beside `genesis_static_stop`).
constexpr std::string_view genesis_routed_failure_stop_declaration =
    "GenesisControlTransfer genesis_routed_failure_stop(GenesisRuntimeStop *stop, const GenesisInstructionProvenance *source, "
    "uint32_t address, GenesisAccessWidth width, GenesisAccessDirection direction)";

std::string shard_helper_linkage(bool sharded, std::string text) {
  if (sharded && text.starts_with("static ")) text.insert(7U, "inline ");
  return text;
}
std::string genesis_unit_declaration(std::string_view prefix, std::uint32_t address) {
  std::ostringstream name;
  name << "GenesisControlTransfer " << prefix << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << address
       << "(GenesisRuntime *runtime)";
  return name.str();
}
using Address = std::uint32_t;
std::string hex(std::uint64_t value, unsigned width) { std::ostringstream out; out << "0x" << std::uppercase << std::hex << std::setw(static_cast<int>(width)) << std::setfill('0') << value; return out.str(); }
// SEG-020-T003: generation-time-only (`--provenance-diagnostics`) execution-history
// hooks. When false the emitted C is byte-identical to the diagnostics-off output.
thread_local bool g_execution_history_hooks = false;
struct ExecutionHistoryHooksScope {
  explicit ExecutionHistoryHooksScope(bool enabled) : previous(g_execution_history_hooks) {
    g_execution_history_hooks = enabled;
  }
  ~ExecutionHistoryHooksScope() { g_execution_history_hooks = previous; }
  bool previous;
};
// SEG-022-T011: see ImmutableRomAotBodyFactoringScope (genesis_frontend.hpp).
thread_local bool g_aot_body_factoring = true;
const char *history_transfer_kind(M68kIrKind kind) {
  switch (kind) {
  case M68kIrKind::branch_ne_short: case M68kIrKind::branch_always_short: case M68kIrKind::general_branch:
  case M68kIrKind::dbcc_loop: return "GENESIS_HISTORY_TRANSFER_DIRECT";
  case M68kIrKind::jump_general: return "GENESIS_HISTORY_TRANSFER_COMPUTED";
  case M68kIrKind::call_general: case M68kIrKind::bsr_call: return "GENESIS_HISTORY_TRANSFER_CALL";
  case M68kIrKind::return_from_subroutine: case M68kIrKind::return_from_exception:
  case M68kIrKind::return_restore_condition_codes:  // SEG-021-T019: RTR
    return "GENESIS_HISTORY_TRANSFER_RETURN";
  default: return "GENESIS_HISTORY_TRANSFER_NONE";
  }
}
// Retire callee + leading arguments: the classic call when hooks are off, else
// the `_at` variant with statically-known retired/fallthrough PCs (never decoded at runtime).
std::string retire_call_open(std::uint32_t address, std::uint32_t length, M68kIrKind kind) {
  if (!g_execution_history_hooks) return "genesis_runtime_retire_m68k_instruction(runtime, ";
  return "genesis_runtime_retire_m68k_instruction_at(runtime, UINT32_C(" + hex(address, 8) + "), UINT32_C(" +
         hex(address + length, 8) + "), " + history_transfer_kind(kind) + ", ";
}
// Same completed-instruction boundary, but with a producer-owned terminal
// result that must win over asynchronous interrupt admission. Device time and
// checkpoint/history accounting still complete inside the runtime call.
std::string retire_before_stop_call_open(std::uint32_t address, std::uint32_t length, M68kIrKind kind) {
  if (!g_execution_history_hooks) return "genesis_runtime_retire_m68k_instruction_before_stop(runtime, ";
  return "genesis_runtime_retire_m68k_instruction_at_before_stop(runtime, UINT32_C(" + hex(address, 8) +
         "), UINT32_C(" + hex(address + length, 8) + "), " + history_transfer_kind(kind) + ", ";
}
bool same(const M68kProgramAddress &a, const M68kProgramAddress &b) { return a.space == b.space && a.value == b.value; }
bool less(const M68kProgramAddress &a, const M68kProgramAddress &b) { return a.space != b.space ? static_cast<unsigned>(a.space) < static_cast<unsigned>(b.space) : a.value < b.value; }
[[maybe_unused]] bool less(const BlockId &a, const BlockId &b) { return less(a.entry, b.entry); }
// SEG-007-T113 / ADR 0008: single source of truth shared byte-for-byte with
// the generated-runtime provenance ABI (GENESIS_MAX_RAW_BYTES) via the
// translation-time/runtime address-space contract. Any decoded instruction
// longer than this still fails closed here before emission.
constexpr std::size_t genesis_frontier_max_raw_bytes = SEGARECOMP_GENESIS_MAX_RAW_INSTRUCTION_BYTES;
constexpr std::size_t genesis_frontier_max_name_length = 64U;
constexpr std::size_t genesis_frontier_max_mapping_claims = 4U;

// SEG-022-T007: compact provenance/mapping metadata. Every stop-provenance
// datum previously emitted as a long run of per-field assignments is now
// installed by two small generated helpers (declared in the same generated
// unit family as `genesis_attach_route_provenance`) or, for the per-instruction
// route lookup, by one immutable mapping table + one sorted immutable record
// table. Values are byte-identical to the former assignments.
constexpr const char *compact_provenance_helper_declarations =
    "void genesis_set_mapping_claim(GenesisProvenance *provenance, uint8_t index, const char *name, uint8_t name_length, uint32_t target_begin, uint32_t target_end, uint64_t image_begin, uint64_t image_end)";
constexpr const char *compact_provenance_helper_declarations_2 =
    "void genesis_set_fetch_access(GenesisProvenance *provenance, uint32_t address, const uint8_t *raw_bytes, uint8_t raw_byte_count)";

std::string compact_provenance_helper_definitions() {
  std::ostringstream out;
  out << compact_provenance_helper_declarations << " {\n"
      << "  uint8_t byte;\n"
      << "  GenesisMappingClaim *claim = &provenance->mapping_claims[index];\n"
      << "  claim->name_length = name_length;\n"
      << "  for (byte = 0U; byte < name_length; ++byte) claim->name[byte] = name[byte];\n"
      << "  claim->target_begin = target_begin;\n"
      << "  claim->target_end = target_end;\n"
      << "  claim->image_begin = image_begin;\n"
      << "  claim->image_end = image_end;\n"
      << "}\n"
      << compact_provenance_helper_declarations_2 << " {\n"
      << "  uint8_t byte;\n"
      << "  provenance->bus_access_count = UINT8_C(1);\n"
      << "  provenance->bus_accesses[0].ordinal = UINT64_C(0);\n"
      << "  provenance->bus_accesses[0].kind = GENESIS_BUS_INSTRUCTION_READ;\n"
      << "  provenance->bus_accesses[0].address = address;\n"
      << "  provenance->bus_accesses[0].raw_byte_count = raw_byte_count;\n"
      << "  provenance->bus_accesses[0].region = GENESIS_REGION_RAW_CARTRIDGE_ROM;\n"
      << "  for (byte = 0U; byte < raw_byte_count; ++byte) provenance->bus_accesses[0].raw_bytes[byte] = raw_bytes[byte];\n"
      << "}\n";
  return out.str();
}

// C string literal with a 3-digit octal escape per byte: exact for any byte.
std::string c_octal_string_literal(const std::string &bytes) {
  std::ostringstream out;
  out << '"';
  for (const char raw_char : bytes) {
    const auto c = static_cast<unsigned>(static_cast<unsigned char>(raw_char));
    out << '\\' << static_cast<char>('0' + (c >> 6U)) << static_cast<char>('0' + ((c >> 3U) & 7U))
        << static_cast<char>('0' + (c & 7U));
  }
  out << '"';
  return out.str();
}

// One `genesis_set_mapping_claim` call statement (no trailing newline).
std::string compact_mapping_claim_call(const std::string &indent, const std::string &provenance_expr,
                                       std::size_t index, const MappingClaim &claim) {
  std::ostringstream out;
  out << indent << "genesis_set_mapping_claim(" << provenance_expr << ", UINT8_C(" << index << "), "
      << c_octal_string_literal(claim.name) << ", UINT8_C(" << claim.name.size() << "), UINT32_C("
      << hex(claim.target_begin.value, 8) << "), UINT32_C(" << hex(claim.target_end.value, 8)
      << "), UINT64_C(" << claim.image_begin.value << "), UINT64_C(" << claim.image_end.value << "));\n";
  return out.str();
}

std::string compact_fetch_access_call(const std::string &indent, const std::string &provenance_expr,
                                      const std::string &address_expr, const std::vector<std::uint8_t> &raw) {
  std::ostringstream out;
  out << indent << "genesis_set_fetch_access(" << provenance_expr << ", " << address_expr
      << ", (const uint8_t[]){";
  for (std::size_t byte = 0; byte < raw.size(); ++byte) out << (byte == 0U ? "" : ", ") << "UINT8_C(" << hex(raw[byte], 2) << ")";
  out << "}, UINT8_C(" << raw.size() << "));\n";
  return out.str();
}

struct RouteProvenanceRecord {
  std::uint32_t address;
  const MappingClaim *mapping;
  const std::vector<std::uint8_t> *raw;
};

// Emits the body of `genesis_attach_route_provenance` (function header already
// written by the caller, closing brace written by the caller) as immutable
// tables plus a bounded binary search. First record for an address wins,
// matching the former first-match `if` chain. Emits the tables just before
// the function via `tables_out`.
void emit_compact_route_provenance(std::ostringstream &tables_out, std::ostringstream &body_out,
                                   std::vector<RouteProvenanceRecord> records) {
  std::stable_sort(records.begin(), records.end(),
                   [](const RouteProvenanceRecord &l, const RouteProvenanceRecord &r) { return l.address < r.address; });
  records.erase(std::unique(records.begin(), records.end(),
                            [](const RouteProvenanceRecord &l, const RouteProvenanceRecord &r) { return l.address == r.address; }),
                records.end());
  if (records.empty()) { body_out << "  (void)stop;\n  (void)source;\n"; return; }
  std::vector<const MappingClaim *> mappings;
  std::vector<std::size_t> mapping_ids;
  for (const auto &record : records) {
    std::size_t id = mappings.size();
    for (std::size_t i = 0; i < mappings.size(); ++i) {
      const auto &m = *mappings[i]; const auto &c = *record.mapping;
      if (m.name == c.name && m.target_begin.value == c.target_begin.value && m.target_end.value == c.target_end.value &&
          m.image_begin.value == c.image_begin.value && m.image_end.value == c.image_end.value) { id = i; break; }
    }
    if (id == mappings.size()) mappings.push_back(record.mapping);
    mapping_ids.push_back(id);
  }
  tables_out << "typedef struct GenesisRouteMapping { const char *name; uint8_t name_length; uint32_t target_begin; uint32_t target_end; uint64_t image_begin; uint64_t image_end; } GenesisRouteMapping;\n"
             << "typedef struct GenesisRouteRecord { uint32_t address; uint32_t mapping; uint8_t raw_byte_count; uint8_t raw_bytes[GENESIS_MAX_RAW_BYTES]; } GenesisRouteRecord;\n"
             << "static const GenesisRouteMapping genesis_route_mappings[] = {\n";
  for (const auto *m : mappings)
    tables_out << "  { " << c_octal_string_literal(m->name) << ", UINT8_C(" << m->name.size() << "), UINT32_C("
               << hex(m->target_begin.value, 8) << "), UINT32_C(" << hex(m->target_end.value, 8) << "), UINT64_C("
               << m->image_begin.value << "), UINT64_C(" << m->image_end.value << ") },\n";
  tables_out << "};\nstatic const GenesisRouteRecord genesis_route_records[] = {\n";
  for (std::size_t i = 0; i < records.size(); ++i) {
    tables_out << "  { UINT32_C(" << hex(records[i].address, 8) << "), UINT32_C(" << mapping_ids[i] << "), UINT8_C("
               << records[i].raw->size() << "), {";
    for (std::size_t b = 0; b < records[i].raw->size(); ++b)
      tables_out << (b == 0U ? "" : ", ") << "UINT8_C(" << hex((*records[i].raw)[b], 2) << ")";
    tables_out << "} },\n";
  }
  tables_out << "};\n";
  body_out << "  size_t low = 0U;\n  size_t high = sizeof genesis_route_records / sizeof genesis_route_records[0];\n"
           << "  while (low < high) {\n"
           << "    const size_t middle = low + (high - low) / 2U;\n"
           << "    if (genesis_route_records[middle].address < source->source_address) low = middle + 1U; else high = middle;\n"
           << "  }\n"
           << "  if (low < sizeof genesis_route_records / sizeof genesis_route_records[0] &&\n"
           << "      genesis_route_records[low].address == source->source_address) {\n"
           << "    const GenesisRouteRecord *record = &genesis_route_records[low];\n"
           << "    const GenesisRouteMapping *mapping = &genesis_route_mappings[record->mapping];\n"
           << "    stop->provenance.mapping_claim_count = UINT8_C(1);\n"
           << "    genesis_set_mapping_claim(&stop->provenance, UINT8_C(0), mapping->name, mapping->name_length, mapping->target_begin, mapping->target_end, mapping->image_begin, mapping->image_end);\n"
           << "    genesis_set_fetch_access(&stop->provenance, source->source_address, record->raw_bytes, record->raw_byte_count);\n"
           << "  }\n";
}
constexpr std::size_t general_startup_frontier_access_record_limit = 4U;
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
bool valid_bridge_rom_sha256(std::string_view value) {
  return value.size() == 64U && std::all_of(value.begin(), value.end(), [](char character) {
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
  });
}
std::optional<std::string_view> c4_access_width(M68kMemoryAccessWidth value) {
  switch (value) {
  case M68kMemoryAccessWidth::byte: return "GENESIS_ACCESS_BYTE";
  case M68kMemoryAccessWidth::word: return "GENESIS_ACCESS_WORD";
  case M68kMemoryAccessWidth::long_word: return "GENESIS_ACCESS_LONG";
  }
  return std::nullopt;
}
std::optional<std::string_view> c4_access_direction(M68kMemoryAccessDirection value) {
  switch (value) {
  case M68kMemoryAccessDirection::read: return "GENESIS_ACCESS_READ";
  case M68kMemoryAccessDirection::write: return "GENESIS_ACCESS_WRITE";
  }
  return std::nullopt;
}
// SEG-007-T032: JSON name vocabulary for the sanitized controller-I/O access
// shape projection. These names are the documented, stable serialized
// vocabulary (see the SEG-007-T032 backlog record Evidence); nothing
// else in the codebase names these values for JSON.
[[maybe_unused]] std::string_view m68k_memory_access_width_json_name(M68kMemoryAccessWidth value) {
  switch (value) {
  case M68kMemoryAccessWidth::byte: return "byte";
  case M68kMemoryAccessWidth::word: return "word";
  case M68kMemoryAccessWidth::long_word: return "long_word";
  }
  return "byte";
}
[[maybe_unused]] std::string_view m68k_memory_access_direction_json_name(M68kMemoryAccessDirection value) {
  return value == M68kMemoryAccessDirection::write ? "write" : "read";
}
[[maybe_unused]] std::string_view controller_io_access_shape_mismatch_json_name(ControllerIoAccessShapeMismatch value) {
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
[[maybe_unused]] std::string_view controller_io_target_register_class_json_name(ControllerIoTargetRegisterClass value) {
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
[[maybe_unused]] std::string_view m68k_opcode_line_name(std::uint16_t word) noexcept {
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
bool runtime_frontier_eligible(const FrontendAnalysis &prefix, const FrontendRejected &diagnostic,
                                    GenesisFrontierClass frontier_class,
                                    const std::optional<M68kMemoryAccessRequest> &access,
                                    const std::set<Address> &sibling_frontier_addresses) {
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
    // discriminator already restricts the caller's classification to that
    // case).
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
        // SEG-007-T183 / ADR-0028 §8: this emission-side mirror of
        // `runtime_frontier_eligible` (platforms/genesis/machine/src/frontend.cpp) is
        // re-run per exit at C4 build time against the very same `prefix`
        // (`accepted_prefix`), which already carries the validated
        // `semantic_partition_boundary_addresses` set -- read directly off
        // it rather than threading a redundant extra parameter, since this
        // copy (unlike the discovery-side one) always receives the full
        // `FrontendAnalysis` already.
        (!retained_entries.contains(edge.target.value) && edge.target.value != frontier_address &&
         !sibling_frontier_addresses.contains(edge.target.value) &&
         !prefix.semantic_partition_boundary_addresses.contains(edge.target.value))) return false;
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
    // SEG-007-T181 / ADR-0027: a fallthrough_continuation edge is the
    // address-order block adjacency generated C already performs at a synthesized
    // partition boundary -- validated exactly like a plain `fallthrough`, no
    // branch/jump emitted, no call identity.
    case M68kStaticEdgeKind::fallthrough_continuation:
    case M68kStaticEdgeKind::direct_branch:
      if (edge.call) return false;
      break;
    case M68kStaticEdgeKind::indirect_call:
      // SEG-007-T124 / ADR-0009: one candidate member of a proven multi-
      // candidate JSR target set -- validated exactly like `direct_call`
      // above, except its target is one admitted candidate rather than the
      // single folded target (there is no `effect.direct_target` for this
      // non-foldable EA to compare against here).
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
    // SEG-007-T134 correction (ADR-0011 Decision §1): see the identical fix
    // and rationale in the `machine/genesis/frontend.cpp` mirror of this
    // function. This loop no longer requires a `return_to_continuation` edge
    // sourced specifically at this frame's own callee-entry-block terminal;
    // the per-edge switch above already fully validates whichever such
    // edges ARE present.
    if (caller == decoded_by_address.end() || !same_provenance(frame.call.caller, caller->second->provenance) ||
        (caller->second->kind != M68kInstructionKind::jsr && caller->second->kind != M68kInstructionKind::bsr) ||
        frame.call.continuation.space != TargetAddressSpace::m68k_program ||
        frame.call.callee.space != TargetAddressSpace::m68k_program || frame.call.continuation.value != continuation ||
        !has_call_edge || frame_count != 1U) return false;
  }
  // SEG-007-T242 / ADR-0039 ("successor freeze 1"): see the identical
  // removal and rationale in the `machine/genesis/frontend.cpp` mirror of
  // this function. The final entry-rooted static-knowledge reachability BFS
  // is removed; missing static graph reachability/ownership alone is no
  // longer, by itself, sufficient to reject an otherwise structurally
  // valid, provenance-consistent frontier candidate. Every check above is
  // unchanged and still fails closed.
  return true;
}

std::vector<const MappingClaim *> claims(const std::vector<MappingClaim> &mappings, Address pc) {
  std::vector<const MappingClaim *> out; for (const auto &m : mappings) if (pc >= m.target_begin.value && pc < m.target_end.value) out.push_back(&m); return out;
}

} // namespace

std::string emit_m68k_general_startup_runtime_block_c(const FrontendAnalysis &analysis) {
  // C2 intentionally recognizes only the project-authored closed MOVEQ/BRA.S
  // fixture. Its branch returns to the entry, so every accepted operation is
  // retained in this one block and lowered through the shared operation owner.
  if (analysis.profile != M68kFrontendProfile::general_startup || !analysis.startup_ingress ||
      analysis.static_blocks.size() != 1U || analysis.static_edges.size() != 1U ||
      !analysis.static_frames.empty() || analysis.decoded.size() != 2U || analysis.ir.size() != 2U)
    return "/* translation rejected: incomplete C2 general-startup block */\n";
  const auto &block = analysis.static_blocks.front();
  const auto &moveq = analysis.ir.front();
  const auto &branch = analysis.ir.back();
  const auto &branch_edge = analysis.static_edges.front();
  if (block.id.entry.space != TargetAddressSpace::m68k_program ||
      block.id.entry.value != analysis.startup_ingress->entry.value ||
      block.instructions.size() != 2U || moveq.kind != M68kIrKind::write_moveq ||
      branch.kind != M68kIrKind::general_branch || branch_edge.kind != M68kStaticEdgeKind::direct_branch ||
      branch_edge.target.space != TargetAddressSpace::m68k_program ||
      branch_edge.target.value != block.id.entry.value ||
      moveq.provenance.source.address.value != block.id.entry.value ||
      moveq.provenance.length.value != 2U || branch.provenance.source.address.value != block.id.entry.value + 2U ||
      branch_edge.source_instruction.source.address.value != branch.provenance.source.address.value)
    return "/* translation rejected: unsupported C2 general-startup block */\n";

  std::ostringstream out;
  out << emit_genesis_runtime_c11_include()
      << "static GenesisControlTransfer genesis_block_" << std::uppercase << std::hex << std::setw(8)
      << std::setfill('0') << block.id.entry.value << "(GenesisRuntime *runtime) {\n";
  GenesisM68kEmissionContext memory{};
  memory.execution_history_hooks = g_execution_history_hooks;
  memory.program_counter = "runtime->pc";
  out << emit_m68k_operation_c(moveq, "runtime->d", "runtime->sr", "  ", &memory);
  out << emit_m68k_operation_c(branch, "runtime->d", "runtime->sr", "  ", &memory);
  out << "  GenesisControlTransfer transfer = {0};\n"
      << "  transfer.kind = GENESIS_CONTINUE_AT_PC;\n"
      << "  transfer.next_pc = runtime->pc;\n"
      << "  return transfer;\n"
      << "}\n";
  return out.str();
}

namespace {
enum class M68kGeneralStartupBlockEmissionPolicy {
  legacy_c3,
  bridge_extended,
};
std::optional<std::map<Address, const FrontendAnalysis::ImmutableRomAotEntry *>>
validated_immutable_rom_aot_entries(const FrontendAnalysis &analysis);
// SEG-022-T011: opt-in exact factoring of one immutable-ROM AOT body (bridge route only).
struct AotBodyFactoring {
  // Group A: a failed routed access returns through the generated `genesis_routed_failure_stop`.
  bool route_failure = false;
  // Group B: when non-empty, the body is emitted WITHOUT its function/label header and closing brace, and
  // every spelling of the entry's own instruction provenance is this `const GenesisInstructionProvenance *`
  // identifier instead of an inline literal, so identical bodies of different entries are byte-identical
  // and can share one statically selected generated helper.
  std::string_view source_symbol;
  // With an empty `source_symbol`: emit the body as a bare `{ ... }` compound statement (the caller owns the
  // function header or entry label) instead of a standalone function or labelled block.
  bool bare_block = false;
};
std::string emit_immutable_rom_aot_body(const FrontendAnalysis::ImmutableRomAotEntry &entry,
                                         const std::vector<std::uint32_t> &runtime_return_targets,
                                         const std::vector<std::uint32_t> &unrepresented_exact_pcs,
                                         const std::vector<std::uint32_t> &indirect_candidate_targets = {},
                                         bool use_shared_compiled_entry_lookup = false,
                                         std::string_view owner_entry_label = {},
                                         const AotBodyFactoring &factoring = {});
std::optional<std::vector<Address>> immutable_rom_aot_exact_pc_obligations(
    const FrontendAnalysis::ImmutableRomAotEntry &entry);
std::optional<std::map<Address, std::vector<Address>>> immutable_rom_aot_unrepresented_exact_pcs(
    const std::map<Address, const FrontendAnalysis::ImmutableRomAotEntry *> &entries,
    const std::set<Address> &represented_exact_pcs);

std::string emit_m68k_general_startup_runtime_c_with_policy_to(
    std::ostream &out, std::string_view header,
    const FrontendAnalysis &analysis, M68kGeneralStartupBlockEmissionPolicy policy) {
  // C3 emits a finite, wholly static subset of a validated general-startup
  // program.  It has no cardinality gate: every accepted static block gets
  // one emitted function and one dispatcher identity.
  if (analysis.profile != M68kFrontendProfile::general_startup || !analysis.startup_ingress ||
      analysis.static_blocks.empty() || !analysis.static_frames.empty() ||
      analysis.decoded.empty() || analysis.decoded.size() != analysis.ir.size())
    return "/* translation rejected: incomplete C3 general-startup program */\n";

  const auto aot_entries = validated_immutable_rom_aot_entries(analysis);
  if (!aot_entries) return "/* translation rejected: invalid immutable-ROM AOT entry */\n";
  std::map<Address, const M68kDecodedInstruction *> decoded;
  std::map<Address, const M68kIrOperation *> operations;
  for (std::size_t index = 0; index < analysis.decoded.size(); ++index) {
    const auto &instruction = analysis.decoded[index];
    const auto &operation = analysis.ir[index];
    const auto address = instruction.provenance.source.address;
    auto raw_span_source = instruction.provenance.source;
    // `raw_bytes` is a verified instruction-local span, so decode it at its
    // local offset while preserving its typed CPU/address source. Restore the
    // original image offset only for the identity comparison below; it is
    // provenance, not an index into this local span.
    raw_span_source.image_offset = MoveqImageOffset{0U};
    auto independently_decoded = decode_m68k_instruction(
        instruction.raw_bytes, raw_span_source, M68kDecodeProfile::general_startup);
    auto *redecoded = std::get_if<M68kDecodedInstruction>(&independently_decoded);
    if (redecoded != nullptr)
      redecoded->provenance.source.image_offset = instruction.provenance.source.image_offset;
    if (address.space != TargetAddressSpace::m68k_program || instruction.provenance.length.value < 2U ||
        instruction.raw_bytes.size() != instruction.provenance.length.value || instruction.raw_bytes.size() < 2U ||
        instruction.raw_bytes[0] != instruction.provenance.bytes[0] ||
        instruction.raw_bytes[1] != instruction.provenance.bytes[1] ||
        redecoded == nullptr || !same_decoded(instruction, *redecoded) ||
        !decoded.emplace(address.value, &instruction).second || !operations.emplace(address.value, &operation).second ||
        !same_ir(operation, lift_m68k_instruction(instruction)))
      return "/* translation rejected: invalid C3 operation provenance */\n";
  }
  for (const auto &[address, entry] : *aot_entries) {
    const auto existing = operations.find(address);
    if (existing != operations.end() && !same_ir(*existing->second, entry->operation))
      return "/* translation rejected: conflicting immutable-ROM AOT identity */\n";
  }

  std::map<Address, const M68kStaticBlock *> blocks;
  std::set<Address> bound_operations;
  for (const auto &block : analysis.static_blocks) {
    if (block.id.entry.space != TargetAddressSpace::m68k_program ||
        block.instructions.empty() || block.instructions.front().source.address.value != block.id.entry.value ||
        !blocks.emplace(block.id.entry.value, &block).second)
      return "/* translation rejected: invalid C3 static block entry */\n";
    Address expected_address = block.id.entry.value;
    for (const auto &provenance : block.instructions) {
      const auto found = decoded.find(provenance.source.address.value);
      if (provenance.source.address.space != TargetAddressSpace::m68k_program || found == decoded.end() ||
          !same_provenance(provenance, found->second->provenance) ||
          provenance.source.address.value != expected_address ||
          !bound_operations.insert(provenance.source.address.value).second)
        return "/* translation rejected: invalid C3 static block */\n";
      expected_address = static_cast<Address>(expected_address + provenance.length.value);
    }
  }
  if (bound_operations.size() != decoded.size() ||
      !blocks.contains(analysis.startup_ingress->entry.value))
    return "/* translation rejected: invalid C3 static block */\n";
  std::map<Address, std::vector<Address>> aot_unrepresented_exact_pcs;
  // SEG-021-T027: this exact block/AOT-entry union is already this C3
  // profile's own final compiled-address authority (it is the only set
  // `immutable_rom_aot_unrepresented_exact_pcs` below checks exact PC
  // obligations against); a sorted copy is retained past this scope to
  // double as the runtime-owned dynamic-indirect-control membership
  // authority passed to `emit_immutable_rom_aot_body` -- never a second,
  // parallel target set.
  std::vector<std::uint32_t> c3_represented_addresses;
  {
    std::set<Address> represented;
    for (const auto &[address, block] : blocks) {
      (void)block;
      represented.insert(address);
    }
    for (const auto &[address, entry] : *aot_entries) {
      (void)entry;
      represented.insert(address);
    }
    const auto unrepresented = immutable_rom_aot_unrepresented_exact_pcs(*aot_entries, represented);
    if (!unrepresented)
      return "/* translation rejected: C3 immutable-ROM AOT PC effect has no consistency owner */\n";
    aot_unrepresented_exact_pcs = *unrepresented;
    c3_represented_addresses.assign(represented.begin(), represented.end());
  }

  std::map<Address, const M68kStaticMemoryFact *> ram_move_facts;
  if (policy == M68kGeneralStartupBlockEmissionPolicy::bridge_extended) {
    // The no-completion bridge admits only a single, fully resolved long-word
    // work-RAM operand on a MOVE; it does not grow a second address resolver
    // or a fixture-address exception. C3 retains its original admission and
    // ignores these bridge-only facts.
    for (const auto &fact : analysis.static_memory_facts) {
      const auto instruction = decoded.find(fact.operation.source.address.value);
      if (instruction == decoded.end() ||
          !same_provenance(fact.operation, instruction->second->provenance) ||
          !same_provenance(fact.source_provenance, instruction->second->provenance) ||
          instruction->second->kind != M68kInstructionKind::move ||
          instruction->second->size != M68kMemoryAccessWidth::long_word ||
          fact.region != M68kAbsoluteOperandRegion::synthetic_work_ram ||
          fact.width != M68kMemoryAccessWidth::long_word ||
          !valid_program_address(fact.address) ||
          m68k_startup_absolute_operand_alignment(fact.address.value) ||
          !m68k_startup_ram_range_in_range(fact.address.value, 4U) ||
          !ram_move_facts.emplace(fact.operation.source.address.value, &fact).second)
        return "/* translation rejected: invalid C3 static memory fact */\n";
      const auto &move = *instruction->second;
      const bool source_read = fact.role == M68kStaticMemoryFactRole::source_read &&
          fact.direction == M68kMemoryAccessDirection::read &&
          move.source_ea.mode == M68kEaMode::absolute_long &&
          fact.address.value == m68k_canonical_ea_address(move.source_ea);
      const bool destination_write = fact.role == M68kStaticMemoryFactRole::destination_write &&
          fact.direction == M68kMemoryAccessDirection::write &&
          move.destination_ea.mode == M68kEaMode::absolute_long &&
          fact.address.value == m68k_canonical_ea_address(move.destination_ea);
      if (!source_read && !destination_write)
        return "/* translation rejected: invalid C3 static memory fact */\n";
    }
  }

  std::map<Address, std::vector<const M68kStaticEdge *>> edges_by_source;
  for (const auto &edge : analysis.static_edges) {
    const auto source = edge.source_instruction.source.address;
    if (source.space != TargetAddressSpace::m68k_program || edge.target.space != TargetAddressSpace::m68k_program ||
        edge.call || !decoded.contains(source.value) || !blocks.contains(edge.target.value) ||
        !same_provenance(edge.source_instruction, decoded.at(source.value)->provenance) ||
        (edge.kind != M68kStaticEdgeKind::direct_branch && edge.kind != M68kStaticEdgeKind::fallthrough))
      return "/* translation rejected: C3 edge target is not a static block */\n";
    edges_by_source[source.value].push_back(&edge);
  }

  out << header;
  if (policy == M68kGeneralStartupBlockEmissionPolicy::bridge_extended) {
    // The generated route helper only attaches retained static provenance to a
    // runtime routing failure. It never reconstructs a target address or
    // reads image bytes.
    std::vector<RouteProvenanceRecord> route_records;
    for (const auto &instruction : analysis.decoded) {
      const auto *mapping = select_unique_affine_mapping(analysis.mapping_claims, instruction.provenance);
      if (mapping == nullptr || mapping->name.size() > genesis_frontier_max_name_length ||
          instruction.raw_bytes.size() > genesis_frontier_max_raw_bytes)
        return "/* translation rejected: invalid bridge retained mapping */\n";
      route_records.push_back({static_cast<std::uint32_t>(instruction.provenance.source.address.value), mapping, &instruction.raw_bytes});
    }
    std::ostringstream route_tables;
    std::ostringstream route_body;
    emit_compact_route_provenance(route_tables, route_body, std::move(route_records));
    out << compact_provenance_helper_definitions() << "\n" << route_tables.str()
        << "void genesis_attach_route_provenance(GenesisRuntimeStop *stop, const GenesisInstructionProvenance *source) {\n"
        << route_body.str() << "}\n\n";
  }
  for (const auto &[entry, block] : blocks) {
    const auto &terminal = block->instructions.back();
    const auto terminal_edges = edges_by_source[terminal.source.address.value];
    for (std::size_t index = 0; index + 1U < block->instructions.size(); ++index)
      if (edges_by_source.contains(block->instructions[index].source.address.value))
        return "/* translation rejected: nonterminal C3 static edge */\n";
    const auto terminal_operation = operations.at(terminal.source.address.value);
    if (terminal_operation->kind != M68kIrKind::general_branch)
      return "/* translation rejected: C3 block lacks terminal branch */\n";
    const auto effect = m68k_operation_effect(*terminal_operation);
    if (effect.pc != M68kPcEffectKind::direct_target)
      return "/* translation rejected: unsupported C3 operation */\n";
    std::size_t direct_count{};
    std::size_t fallthrough_count{};
    for (const auto *edge : terminal_edges) {
      if (edge->kind == M68kStaticEdgeKind::direct_branch) {
        ++direct_count;
        if (edge->target.value != effect.direct_target)
          return "/* translation rejected: C3 branch target disagrees with static edge */\n";
      } else {
        ++fallthrough_count;
        if (edge->target.value != terminal.source.address.value + terminal.length.value)
          return "/* translation rejected: C3 fallthrough disagrees with static edge */\n";
      }
    }
    const bool unconditional = terminal_operation->condition == M68kCondition::always;
    if (direct_count != 1U || fallthrough_count != (unconditional ? 0U : 1U) ||
        terminal_edges.size() != direct_count + fallthrough_count)
      return "/* translation rejected: incomplete C3 static edge */\n";

    out << "static GenesisControlTransfer genesis_block_" << std::uppercase << std::hex << std::setw(8)
        << std::setfill('0') << entry << "(GenesisRuntime *runtime) {\n";
    GenesisM68kEmissionContext memory{};
  memory.execution_history_hooks = g_execution_history_hooks;
    memory.program_counter = "runtime->pc";
    memory.address_registers = "runtime->a";
    memory.user_stack_pointer = "runtime->usp";
    for (const auto &provenance : block->instructions) {
      const auto operation = operations.find(provenance.source.address.value);
      if (operation == operations.end() || !same_provenance(operation->second->provenance, provenance))
        return "/* translation rejected: missing C3 block operation */\n";
      switch (operation->second->kind) {
      case M68kIrKind::write_moveq:
        // SEG-007-T086: MOVEQ's C4/write_moveq lowering (emit_m68k_operation_c)
        // already reads operation.destination directly and is fully generic
        // over D0-D7; this policy previously restricted bridge_extended's own
        // emission to D0 only even though nothing downstream required it.
        break;
      case M68kIrKind::subtract_quick_long_d0:
        break;
      case M68kIrKind::subtract_quick:
        if (operation->second->source_ea.mode != M68kEaMode::immediate ||
            operation->second->destination_ea.mode != M68kEaMode::data_register)
          return "/* translation rejected: unsupported C3 operation */\n";
        break;
      case M68kIrKind::load_effective_address:
        // C3 deliberately admits only LEA (An),Am: the shared lowering uses
        // runtime->a only and neither reads RAM nor evaluates an index.
        if (operation->second->source_ea.mode != M68kEaMode::address_indirect ||
            operation->second->destination_ea.mode != M68kEaMode::address_register ||
            operation->second->source_ea.reg > 7U || operation->second->destination_ea.reg > 7U)
          return "/* translation rejected: unsupported C3 operation */\n";
        break;
      case M68kIrKind::write_move: {
        if (policy != M68kGeneralStartupBlockEmissionPolicy::bridge_extended)
          return "/* translation rejected: unsupported C3 operation */\n";
        const auto &move = *decoded.at(provenance.source.address.value);
        const auto fact = ram_move_facts.find(provenance.source.address.value);
        const bool store = move.source_ea.mode == M68kEaMode::data_register && move.source_ea.reg < 8U &&
            move.destination_ea.mode == M68kEaMode::absolute_long && fact != ram_move_facts.end() &&
            fact->second->role == M68kStaticMemoryFactRole::destination_write;
        const bool load = move.source_ea.mode == M68kEaMode::absolute_long &&
            move.destination_ea.mode == M68kEaMode::data_register && move.destination_ea.reg < 8U &&
            fact != ram_move_facts.end() && fact->second->role == M68kStaticMemoryFactRole::source_read;
        if (!store && !load)
          return "/* translation rejected: unsupported C3 operation */\n";
        auto routed = memory;
        routed.runtime_routing = true;
        routed.runtime_object = "runtime";
        if (load)
          routed.test_operand_access = genesis_lowering_access(M68kAbsoluteOperandRegion::synthetic_work_ram);
        out << "#define pc runtime->pc\n"
            << emit_m68k_operation_c(*operation->second, "runtime->d", "runtime->sr", "  ", &routed)
            << "#undef pc\n";
        continue;
      }
      case M68kIrKind::general_branch:
        break;
      default:
        return "/* translation rejected: unsupported C3 operation */\n";
      }
      out << emit_m68k_operation_c(*operation->second, "runtime->d", "runtime->sr", "  ", &memory);
    }
    out << "  GenesisControlTransfer transfer = {0};\n"
        << "  transfer.kind = GENESIS_CONTINUE_AT_PC;\n"
        << "  transfer.next_pc = runtime->pc;\n"
        << "  return transfer;\n"
        << "}\n\n";
  }
  // SEG-007-T246: this C3 profile requires `analysis.static_frames.empty()`
  // (checked at function entry above), so `m68k_operation_is_immutable_rom_
  // aot_safe` never admits `return_from_subroutine` for this profile either
  // (same criterion, `validated_immutable_rom_aot_entries` above) -- an
  // empty return-target vector is always the correct, harmless value here.
  // SEG-022-T007: the legacy C3 profile has no route-provenance helper unit, so it
  // defines the compact provenance helpers itself when it emits any AOT body.
  if (policy != M68kGeneralStartupBlockEmissionPolicy::bridge_extended &&
      std::ranges::any_of(*aot_entries, [&](const auto &candidate) { return !blocks.contains(candidate.first); }))
    out << compact_provenance_helper_definitions() << "\n";
  for (const auto &[address, entry] : *aot_entries)
    if (!blocks.contains(address))
      out << emit_immutable_rom_aot_body(*entry, {}, aot_unrepresented_exact_pcs[address],
                                         c3_represented_addresses);
  out << "static GenesisControlTransfer genesis_dispatch(GenesisRuntime *runtime) {\n";
  for (const auto &[entry, block] : blocks) {
    out << "  if (runtime->pc == UINT32_C(0x" << std::uppercase << std::hex << std::setw(8)
        << std::setfill('0') << entry << ")) return genesis_block_" << std::setw(8)
        << entry << "(runtime);\n";
  }
  for (const auto &[address, entry] : *aot_entries) {
    (void)entry;
    if (!blocks.contains(address))
      out << "  if (runtime->pc == UINT32_C(" << hex(address, 8) << ")) return genesis_aot_"
          << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << address << "(runtime);\n";
  }
  out << "  return genesis_internal_dispatch_inconsistency_stop(runtime);\n"
      << "}\n";
  return {};  // success: text was streamed to `out`; non-empty return is a rejection
}

std::string emit_m68k_general_startup_runtime_c_with_policy(
    const FrontendAnalysis &analysis, M68kGeneralStartupBlockEmissionPolicy policy) {
  std::ostringstream out;
  auto rejection = emit_m68k_general_startup_runtime_c_with_policy_to(out, emit_genesis_runtime_c11_include(), analysis, policy);
  return rejection.empty() ? out.str() : rejection;
}
} // namespace

std::string emit_m68k_general_startup_runtime_c(const FrontendAnalysis &analysis) {
  return emit_m68k_general_startup_runtime_c_with_policy(
      analysis, M68kGeneralStartupBlockEmissionPolicy::legacy_c3);
}

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
std::optional<std::string_view> c4_diagnostic(DirectFlowDiagnostic value) {
  constexpr std::array<std::string_view, 34> names{
      "GENESIS_DIAG_ODD_INSTRUCTION_ADDRESS", "GENESIS_DIAG_UNMAPPED_INSTRUCTION_ADDRESS",
      "GENESIS_DIAG_TRUNCATED_INSTRUCTION", "GENESIS_DIAG_ILLEGAL_INSTRUCTION",
      "GENESIS_DIAG_VALID_BUT_UNSUPPORTED_INSTRUCTION", "GENESIS_DIAG_UNSUPPORTED_INSTRUCTION_FORM",
      "GENESIS_DIAG_ODD_DIRECT_TARGET", "GENESIS_DIAG_CONFLICTING_ADDRESS_MAPPING",
      "GENESIS_DIAG_INVALID_ADDRESS_MAPPING", "GENESIS_DIAG_UNMAPPED_DIRECT_TARGET",
      "GENESIS_DIAG_MID_INSTRUCTION_DIRECT_TARGET", "GENESIS_DIAG_REACHED_UNRESOLVED_DIRECT_EDGE",
      "GENESIS_DIAG_INVALID_FRONTEND_IMAGE_SOURCE_ID", "GENESIS_DIAG_FRONTEND_IMAGE_BYTE_LENGTH_MISMATCH",
      "GENESIS_DIAG_INVALID_MAPPING_CLAIM", "GENESIS_DIAG_VECTOR_FIXTURE_ID_MISMATCH",
      "GENESIS_DIAG_VECTOR_IMAGE_SHA256_MISMATCH", "GENESIS_DIAG_VECTOR_CPU_VARIANT_MISMATCH",
      "GENESIS_DIAG_VECTOR_EXECUTION_ENTRY_SPACE_MISMATCH", "GENESIS_DIAG_EXECUTION_ENTRY_INSIDE_DISCOVERED_INSTRUCTION",
      "GENESIS_DIAG_EXECUTION_ENTRY_INSIDE_DISCOVERED_BLOCK", "GENESIS_DIAG_EXECUTION_ENTRY_NOT_DISCOVERED_BLOCK_START",
      "GENESIS_DIAG_VECTOR_BLOCK_INSTRUCTION_COUNT_MISMATCH", "GENESIS_DIAG_EFFECTIVE_ADDRESS_NOT_24BIT",
      "GENESIS_DIAG_ODD_EFFECTIVE_ADDRESS", "GENESIS_DIAG_ROM_WRITE_PROHIBITED",
      "GENESIS_DIAG_UNMAPPED_DATA_ACCESS", "GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO",
      "GENESIS_DIAG_INVALID_STACK_ALIGNMENT", "GENESIS_DIAG_INVALID_STACK_RANGE",
      "GENESIS_DIAG_RETURN_CONTEXT_MISSING", "GENESIS_DIAG_RETURN_TARGET_MISMATCH",
      "GENESIS_DIAG_STARTUP_GRAPH_MISMATCH", "GENESIS_DIAG_DISCOVERY_BUDGET_EXHAUSTED"};
  const auto ordinal = static_cast<std::size_t>(value);
  return ordinal < names.size() ? std::optional<std::string_view>{names[ordinal]} : std::nullopt;
}
std::optional<std::string_view> c5_cpu_dimensions(M68kCpuFrontierKind frontier) {
  switch (frontier) {
  case M68kCpuFrontierKind::nop: return "GENESIS_CPU_DIMENSIONS_NOP";
  case M68kCpuFrontierKind::stop_immediate_word: return "GENESIS_CPU_DIMENSIONS_STOP_IMMEDIATE_WORD";
  case M68kCpuFrontierKind::reset: return "GENESIS_CPU_DIMENSIONS_RESET";
  case M68kCpuFrontierKind::move_an_to_usp: return "GENESIS_CPU_DIMENSIONS_MOVE_AN_TO_USP";
  case M68kCpuFrontierKind::none: return std::nullopt;
  }
  return std::nullopt;
}
// SEG-007-T142 correction: the previous address-carrying emission
// classifier lived here as a separate, ad hoc reimplementation of
// preflight_m68k_general_startup_c4's own per-operation gap logic (and had
// silently drifted from it -- e.g. it treated branch_ne_short/
// branch_always_short as lowerable while preflight's own `represented()`
// does not). It is replaced by the single shared `classify_m68k_c4_gap_
// shapes` classifier and the exhaustive `c4_lowering_dimension_literal`
// mapper defined immediately before `preflight_m68k_general_startup_c4`
// below, used by both the advisory preflight inventory and each generated
// `genesis_c4_lowering_stop_<addr>`'s own dimension-literal selection, so
// the two can never silently diverge again.
// SEG-007-T064: one uniquely named `genesis_frontier_stop_<8-hex-source-
// address>` function's full text (or std::nullopt on any of the same
// unrepresentable-frontier conditions the single-frontier C4 lowering
// already checked) for exactly one retained exit -- the per-exit
// generalization of the body the single-frontier
// emit_m68k_general_startup_runtime_c used to emit inline. `accepted_prefix`
// is required only for the shared runtime_frontier_eligible re-check every
// exit independently repeats, exactly as the single-frontier path already
// did.
// SEG-007-T208 correction (ADR-0011 Decision §1 / ADR-0024's literal
// call-shaped "unframed Tier-2 call" case): the trailing
// `out_tier2_call_continuation` parameter below reports a call-shaped
// (`is_call == true`) Tier-2-admitted site's own already-computed
// continuation (source address + instruction length, computed identically
// inside this function regardless of which downstream target the
// runtime-computed EA turns out to be) back to the caller. Ordinary
// discovery-time call/frame machinery never sees this site at all (Tier 2 is
// realized entirely at C4 emission time, per ADR-0024), so this continuation
// is never registered as an `M68kStaticCall` frame and never reaches
// `runtime_return_target_set` on its own -- the caller unions this
// already-computed value into that shared whole-program return-target set
// ADR-0011 already assigns to every retained RTS. No target-provenance/
// points-to analysis is needed, since a call's own continuation never
// depends on knowing its callee.
//
// SEG-007-T239 / ADR-0039: this is the one deterministic reusable
// compiled-address existence query for dynamic JMP and JSR, established over
// the existing generation-time `EmittedCodeAddressSet` and the unchanged
// `genesis_dispatch`. It answers exactly one question -- "is this runtime-
// computed 32-bit value the entry address of some block this build actually
// emitted?" -- and nothing else: it never discovers, learns, decodes, or
// admits a target, and its own C11 body always defers control back to the
// caller, which only ever assigns `pc`/pushes a call continuation on a
// positive answer and lets the existing, unchanged `genesis_dispatch` select
// the already-emitted block on the next drive iteration. Both Tier-2 JMP
// (`is_call == false`) and Tier-2 JSR (`is_call == true`) sites already
// called this exact predicate before T239; extracting it here only gives an
// already-shared check its own explicit, reusable identity -- it changes no
// byte of previously generated output. See this task's Evidence for the
// full JMP/JSR existence-predicate inventory and the reasons Tier-1's
// narrower `m68k_indirect_target_member` proven-candidate check and RTS's
// `runtime_return_targets` whole-program continuation check are
// deliberately NOT migrated to this query.
// `function_header` is everything the caller wants emitted between the
// array declaration and the existence-check block (typically the frontier
// stop function's own signature plus its `source`-provenance population
// statements) -- kept as a caller-supplied opaque string so this helper
// contributes no reordering or reformatting of previously generated text.
std::string emit_m68k_compiled_address_existence_check(const std::string &array_name,
                                                         const std::vector<std::uint32_t> &emitted_code_addresses,
                                                         const std::string &function_header,
                                                         const std::string &ea_expr, const std::string &fail_stop,
                                                         bool use_shared_compiled_entry_lookup) {
  std::ostringstream out;
  if (use_shared_compiled_entry_lookup) {
    out << function_header << "  { const uint32_t m68k_indirect_ea = " << ea_expr << ";\n"
        << "    if (genesis_compiled_entry_lookup(m68k_indirect_ea) == NULL)\n"
        << "      return " << fail_stop << ";\n";
    return out.str();
  }
  out << "static const uint32_t " << array_name << "[] = {";
  for (std::size_t index = 0; index < emitted_code_addresses.size(); ++index) {
    if (index != 0U) out << ", ";
    out << "UINT32_C(" << hex(emitted_code_addresses[index], 8) << ")";
  }
  // A C11 array initializer may not be empty; an empty `EmittedCodeAddressSet`
  // (impossible in practice -- the reset entry itself is always a member --
  // but never assumed away here) falls back to a one-element sentinel array
  // containing only the frontier's own unreachable-by-construction address
  // `0xFFFFFFFF`, so membership always and correctly evaluates false without
  // ever compiling an illegal empty initializer.
  if (emitted_code_addresses.empty()) out << "UINT32_C(0xFFFFFFFF)";
  out << "};\n" << function_header << "  { const uint32_t m68k_indirect_ea = " << ea_expr << ";\n"
      << "    if (!m68k_emitted_code_address_member(" << array_name << ", (uint32_t)(sizeof(" << array_name
      << ") / sizeof(" << array_name << "[0])), m68k_indirect_ea))\n"
      << "      return " << fail_stop << ";\n";
  return out.str();
}

// SEG-021-T028: the one Tier-2 computed-control lowering (runtime EA plus
// compiled emitted-code-set membership; never a target fetch/decode), shared by
// a Tier-2 frontier stop and by a retained-block terminal whose finite Tier-1
// set could not be lowered. Returns nullopt for an EA shape Tier 2 cannot own.
std::optional<std::string> build_tier2_computed_control_function(
    const FrontendAnalysis &accepted_prefix, const InstructionProvenance &provenance,
    const M68kEffectiveAddress &ea, bool is_call, const std::string &function_name,
    const std::vector<std::uint32_t> &emitted_code_addresses,
    std::optional<std::uint32_t> *out_tier2_call_continuation) {
  std::ostringstream name_suffix_stream;
  name_suffix_stream << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
                     << provenance.source.address.value;
  const auto name_suffix = name_suffix_stream.str();
  
  // SEG-007-T179 / ADR-0025: Tier-2 now also lowers the pure
  // register-indirect control EA form (`JMP (An)` / `JSR (An)`, mode 2,
  // no displacement/index/extension). A7/SP is refused outright -- the
  // SEG-007-T178 exclusion holds at both tiers; the producer never records
  // an A7 fact, and this is a belt-and-braces guard. All other gates
  // (engagement on a non-empty candidate-root set, emitted-set membership,
  // fail-closed non-member, JSR return-frame push) are shared unchanged
  // between the two forms.
  const bool is_pc_index8 =
      ea.mode == M68kEaMode::pc_index8 && !ea.index_is_address && !ea.index_is_long;
  const bool is_reg_indirect = ea.mode == M68kEaMode::address_indirect && ea.displacement == 0 &&
                               ea.extension_words == 0U && ea.reg < 7U;
  // SEG-021-T011 / ADR-0024/ADR-0025 Tier-2 generalization: the brief
  // address-register-indexed control EA (`JMP (d8,An,Xn)` /
  // `JSR (d8,An,Xn)`), the sibling shape `process_indirect_control_index8`
  // (static_discovery.cpp) always records a Tier-2 fact for -- this
  // project deliberately never attempts a Tier-1 finite-value proof for
  // this combined base+index shape (see that function's own doc
  // comment). Unlike the two existing shapes above, the index register
  // bank/size here is not restricted to word-size Dn: the runtime EA
  // expression below is built generically (An/Dn index, word/long size),
  // mirroring `m68k_emit_runtime_ea_address`'s own `address_index8`
  // formula (libs/codegen/c11/src/m68k.cpp) exactly, because this is a
  // plain runtime register read/compare, not a static finite-value
  // proof with a bounded-domain restriction to honor.
  const bool is_index8 = ea.mode == M68kEaMode::address_index8;
  if (!is_pc_index8 && !is_reg_indirect && !is_index8) return std::nullopt;
  std::string ea_expr;
  if (is_pc_index8) {
    const auto base = static_cast<std::uint32_t>(static_cast<std::int64_t>(ea.pc_base_address) +
                                                   static_cast<std::int64_t>(ea.displacement));
    const auto index_expr =
        std::string("runtime->d[") + std::to_string(static_cast<unsigned>(ea.index_reg)) + "]";
    std::ostringstream ea_build;
    ea_build << "UINT32_C(" << hex(base, 8) << ") + (uint32_t)(int32_t)(int16_t)(uint16_t)("
             << index_expr << ")";
    ea_expr = ea_build.str();
  } else if (is_index8) {
    const auto base_expr = std::string("runtime->a[") + std::to_string(static_cast<unsigned>(ea.reg)) + "]";
    const auto index_bank = ea.index_is_address ? std::string("runtime->a[") : std::string("runtime->d[");
    const auto index_expr = index_bank + std::to_string(static_cast<unsigned>(ea.index_reg)) + "]";
    std::ostringstream ea_build;
    ea_build << "(uint32_t)(" << base_expr << " + ";
    if (ea.index_is_long)
      ea_build << "(int32_t)" << index_expr;
    else
      ea_build << "(int32_t)(int16_t)(uint16_t)" << index_expr;
    ea_build << " + (int32_t)(int8_t)" << static_cast<int>(ea.displacement) << ")";
    ea_expr = ea_build.str();
  } else {
    ea_expr = std::string("runtime->a[") + std::to_string(static_cast<unsigned>(ea.reg)) + "]";
  }
  const auto array_name = "genesis_emitted_code_addresses_" + name_suffix;
  std::ostringstream function_header;
  function_header << "static GenesisControlTransfer " << function_name
                   << "(GenesisRuntime *runtime) {\n"
                   << "  GenesisInstructionProvenance source = {0};\n"
                   << "  source.cpu_variant = GENESIS_CPU_MC68000;\n"
                   << "  source.source_address = UINT32_C(" << hex(provenance.source.address.value, 8) << ");\n"
                   << "  source.image_offset = UINT64_C(" << provenance.source.image_offset.value << ");\n"
                   << "  source.primary_bytes[0] = UINT8_C(" << hex(provenance.bytes[0], 2) << ");\n"
                   << "  source.primary_bytes[1] = UINT8_C(" << hex(provenance.bytes[1], 2) << ");\n"
                   << "  source.length = UINT32_C(" << provenance.length.value << ");\n";
  std::ostringstream fail_stop;
  fail_stop << "genesis_static_stop(GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET, "
               "GENESIS_DIAG_TIER2_COMPUTED_TARGET_NOT_EMITTED, &source, 0U, UINT32_C(0), "
               "GENESIS_ACCESS_LONG, GENESIS_ACCESS_READ)";
  std::ostringstream tier2;
  // SEG-007-T239 / ADR-0039: the one reusable compiled-address existence
  // query, shared unchanged by both Tier-2 JMP (`is_call == false`
  // below) and Tier-2 JSR (`is_call == true` below) -- see this
  // function's own doc comment above.
  tier2 << emit_m68k_compiled_address_existence_check(array_name, emitted_code_addresses,
                                                       function_header.str(), ea_expr, fail_stop.str(),
                                                       !accepted_prefix.immutable_rom_aot_entries.empty());
  if (is_call) {
    const auto continuation =
        static_cast<std::uint32_t>(provenance.source.address.value + provenance.length.value);
    // SEG-007-T208 correction: report this call-shaped Tier-2 site's own
    // genuine continuation back to the caller (see the out-parameter's
    // own doc comment above) -- the identical value already pushed onto
    // the emulated runtime stack a few lines below.
    if (out_tier2_call_continuation != nullptr) *out_tier2_call_continuation = continuation;
    tier2 << "    { uint32_t m68k_continuation = UINT32_C(" << hex(continuation, 8) << ");\n"
          << "      GenesisRuntimeStop m68k_route_stop = {0};\n"
          << "      if ((runtime->a[7] & 1U) != 0U) return genesis_static_stop(GENESIS_STOP_UNSUPPORTED_MEMORY_REGION, GENESIS_DIAG_INVALID_STACK_ALIGNMENT, &source, 0U, UINT32_C(0), GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE);\n"
          << "      if (runtime->a[7] < UINT32_C(" << hex(m68k_startup_ram_begin + 4U, 8) << ") || runtime->a[7] > UINT32_C("
          << hex(m68k_startup_ram_end, 8) << ")) return genesis_static_stop(GENESIS_STOP_UNSUPPORTED_MEMORY_REGION, GENESIS_DIAG_INVALID_STACK_RANGE, &source, 0U, UINT32_C(0), GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE);\n"
          << "      { const uint32_t m68k_new_a7 = runtime->a[7] - UINT32_C(4);\n"
          << "        if (" << (g_execution_history_hooks ? "genesis_route_access_bus(runtime, GENESIS_BUS_STACK_WRITE, " : "genesis_route_access(runtime, ") << "m68k_new_a7, GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE, &m68k_continuation, &m68k_route_stop) != GENESIS_ACCESS_OK) {\n"
          << "          m68k_route_stop.provenance.has_instruction_provenance = 1U; m68k_route_stop.provenance.instruction = source; m68k_route_stop.provenance.has_access = 1U; m68k_route_stop.provenance.access_address = m68k_new_a7; m68k_route_stop.provenance.access_width = GENESIS_ACCESS_LONG; m68k_route_stop.provenance.access_direction = GENESIS_ACCESS_WRITE;\n"
          << "          { GenesisControlTransfer transfer = {0}; transfer.kind = GENESIS_STOP; transfer.stop = m68k_route_stop; return transfer; }\n"
          << "        }\n"
          << "        runtime->a[7] = m68k_new_a7;\n"
          << "      }\n"
          << "    }\n";
  }
  tier2 << "    { GenesisControlTransfer transfer = {0};\n"
        << "      transfer.kind = GENESIS_CONTINUE_AT_PC;\n"
        << "      transfer.next_pc = m68k_indirect_ea;\n"
        << "      return transfer;\n"
        << "    }\n"
        << "  }\n"
        << "}\n";
  return tier2.str();
}

std::optional<std::string> build_genesis_frontier_stop_function(
    const FrontendAnalysis &accepted_prefix, const UnresolvedFrontier &frontier,
    const std::set<Address> &sibling_frontier_addresses,
    const std::vector<std::uint32_t> &emitted_code_addresses = {},
    std::optional<std::uint32_t> *out_tier2_call_continuation = nullptr) {
  const auto &diagnostic = frontier.diagnostic;
  const auto stop_class = c4_stop_class(frontier.class_);
  const bool known_but_unemitted = frontier.class_ == GenesisFrontierClass::known_but_unemitted_target;
  // SEG-007-T064: known_but_unemitted_target never lowers its underlying
  // diagnostic.category through c4_diagnostic -- that category is host-side
  // bookkeeping only for this class (the coarse "known but unemitted"
  // meaning is carried entirely by stop_class/frontier.class_). The literal
  // generation-only GENESIS_DIAG_KNOWN_BUT_UNEMITTED_TARGET enumerator is
  // used unconditionally instead.
  const auto category = known_but_unemitted
                             ? std::optional<std::string_view>{"GENESIS_DIAG_KNOWN_BUT_UNEMITTED_TARGET"}
                             : c4_diagnostic(diagnostic.category);
  if (!stop_class || !category ||
      !runtime_frontier_eligible(accepted_prefix, diagnostic, frontier.class_, frontier.access,
                                  sibling_frontier_addresses))
    return std::nullopt;
  if ((diagnostic.mapping_claims &&
       (diagnostic.mapping_claims->size() > genesis_frontier_max_mapping_claims ||
        std::any_of(diagnostic.mapping_claims->begin(), diagnostic.mapping_claims->end(),
                    [](const MappingClaim &claim) { return claim.name.size() > genesis_frontier_max_name_length; }))) ||
      diagnostic.accesses.size() > general_startup_frontier_access_record_limit ||
      std::any_of(diagnostic.accesses.begin(), diagnostic.accesses.end(),
                  [](const StartupBusRecord &access) { return access.bytes.size() > genesis_frontier_max_raw_bytes; }))
    return std::nullopt;
  if (!diagnostic.provenance) return std::nullopt;
  const auto cpu_dimensions = c5_cpu_dimensions(classify_m68k_cpu_frontier(*diagnostic.provenance));
  if (frontier.class_ == GenesisFrontierClass::unsupported_cpu_form && !cpu_dimensions) return std::nullopt;
  // C4 currently retains only the discovery instruction-fetch observation.
  // Do not serialize a lossy guessed region or a bus record detached from the
  // frontier instruction: either condition is a build-time rejection.
  for (const auto &bus : diagnostic.accesses) {
    if (bus.kind != StartupBusKind::instruction_read || bus.region != "raw_cartridge_rom" ||
        !same(bus.address, diagnostic.provenance->source.address) ||
        !same_provenance(bus.instruction, *diagnostic.provenance) ||
        bus.bytes.size() != diagnostic.provenance->length.value || bus.bytes.size() < 2U ||
        bus.bytes[0] != diagnostic.provenance->bytes[0] || bus.bytes[1] != diagnostic.provenance->bytes[1])
      return std::nullopt;
  }
  if (frontier.access &&
      (!frontier.access->source_provenance || !same_provenance(*frontier.access->source_provenance, *diagnostic.provenance)))
    return std::nullopt;
  const auto &provenance = *diagnostic.provenance;
  // The function name's own hex address suffix is built into its own local
  // stream first, and only its already-formatted string is appended to the
  // body stream below -- this avoids leaking std::hex/std::uppercase
  // formatting state into the plain-decimal numeric literals (mapping-claim/
  // bus-access counts, and so on) the body still emits further down.
  std::ostringstream name_suffix_stream;
  name_suffix_stream << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
                      << provenance.source.address.value;
  const auto name_suffix = name_suffix_stream.str();
  // SEG-007-T174 / ADR-0024: Tier 2's own dispatch. A `reached_unresolved_
  // direct_edge` frontier whose exact source instruction matches a retained
  // `M68kUnprovenIndirectControlEaSet` fact (source provenance AND decoded
  // control EA both equal -- never just an address coincidence) is emitted
  // as a faithful runtime EA computation plus a binary-search membership
  // guard against the generation-time `EmittedCodeAddressSet`, in place of
  // the generic unconditional-stop body below. This never fetches or
  // decodes a target instruction: it only ever compares one computed
  // 32-bit integer against a compiled-in address table, exactly like Tier
  // 1's own `m68k_indirect_target_member` guard. Every other frontier class/
  // category keeps the generic body completely unchanged.
  // SEG-007-T174 / ADR-0024 Non-goal / Acceptance: the raw/unannotated route
  // (no `--external-hints`, so `validated_code_entry_candidate_roots` is
  // always empty) must stay byte-for-byte unaffected -- not merely "still
  // correct" but textually IDENTICAL, including the diagnostic category an
  // unprovable computed indirect site's frontier stop reports. Tier 2
  // therefore additionally requires at least one validated, Ghidra-(or
  // equivalent-)assisted code-entry candidate root to exist ANYWHERE in this
  // program before it ever engages for ANY site -- the whole mechanism is
  // additive to an assisted build, never a silent behavior change for a
  // program that never opted into candidate-assisted discovery at all, even
  // though `M68kUnprovenIndirectControlEaSet` facts are always recorded
  // (harmlessly) regardless of this gate.
  if ((!accepted_prefix.validated_code_entry_candidate_roots.empty() ||
       !accepted_prefix.immutable_rom_aot_entries.empty()) &&
      frontier.class_ == GenesisFrontierClass::unresolved_indirect_target &&
      diagnostic.category == DirectFlowDiagnostic::reached_unresolved_direct_edge) {
    const M68kUnprovenIndirectControlEaSet *unproven = nullptr;
    for (const auto &candidate : accepted_prefix.unproven_indirect_control_ea_sets) {
      if (!same_provenance(candidate.source_instruction, provenance)) continue;
      if (unproven != nullptr) return std::nullopt;  // ambiguous: more than one fact for this exact source
      unproven = &candidate;
    }
    if (unproven != nullptr) {
      const auto tier2_text = build_tier2_computed_control_function(
          accepted_prefix, provenance, unproven->control_ea, unproven->is_call,
          "genesis_frontier_stop_" + name_suffix, emitted_code_addresses, out_tier2_call_continuation);
      if (!tier2_text) return std::nullopt;
      return tier2_text;
    }
  }
  std::ostringstream out;
  out << "static GenesisControlTransfer genesis_frontier_stop_" << name_suffix << "(GenesisRuntime *runtime) {\n"
      << "  GenesisControlTransfer transfer = {0};\n"
      << "  (void)runtime;\n  transfer.kind = GENESIS_STOP;\n  transfer.stop.stop_class = " << *stop_class
      << ";\n  transfer.stop.diagnostic_category = " << *category << ";\n"
      << "  transfer.stop.provenance.has_instruction_provenance = 1U;\n"
      << "  transfer.stop.provenance.instruction.cpu_variant = GENESIS_CPU_MC68000;\n"
      << "  transfer.stop.provenance.instruction.source_address = UINT32_C(" << hex(provenance.source.address.value, 8) << ");\n"
      << "  transfer.stop.provenance.instruction.image_offset = UINT64_C(" << provenance.source.image_offset.value << ");\n"
      << "  transfer.stop.provenance.instruction.primary_bytes[0] = UINT8_C(" << hex(provenance.bytes[0], 2) << ");\n"
      << "  transfer.stop.provenance.instruction.primary_bytes[1] = UINT8_C(" << hex(provenance.bytes[1], 2) << ");\n"
      << "  transfer.stop.provenance.instruction.length = UINT32_C(" << provenance.length.value << ");\n";
  if (frontier.access) {
    const auto &access = *frontier.access;
    const auto width = c4_access_width(access.width);
    const auto direction = c4_access_direction(access.direction);
    if (!width || !direction || !valid_program_address(access.address)) return std::nullopt;
    out << "  transfer.stop.provenance.has_access = 1U;\n"
        << "  transfer.stop.provenance.access_address = UINT32_C(" << hex(access.address.value, 8) << ");\n"
        << "  transfer.stop.provenance.access_width = " << *width << ";\n"
        << "  transfer.stop.provenance.access_direction = " << *direction << ";\n";
  }
  if (diagnostic.mapping_claims) {
    out << "  transfer.stop.provenance.mapping_claim_count = UINT8_C(" << diagnostic.mapping_claims->size() << ");\n";
    for (std::size_t index = 0; index < diagnostic.mapping_claims->size(); ++index)
      out << compact_mapping_claim_call("  ", "&transfer.stop.provenance", index, diagnostic.mapping_claims->at(index));
  }
  out << "  transfer.stop.provenance.bus_access_count = UINT8_C(" << diagnostic.accesses.size() << ");\n";
  for (std::size_t index = 0; index < diagnostic.accesses.size(); ++index) {
    const auto &bus = diagnostic.accesses[index];
    const auto kind = bus.kind == StartupBusKind::instruction_read ? "GENESIS_BUS_INSTRUCTION_READ" :
                      bus.kind == StartupBusKind::data_read ? "GENESIS_BUS_DATA_READ" :
                      bus.kind == StartupBusKind::data_write ? "GENESIS_BUS_DATA_WRITE" :
                      bus.kind == StartupBusKind::stack_read ? "GENESIS_BUS_STACK_READ" : "GENESIS_BUS_STACK_WRITE";
    out << "  transfer.stop.provenance.bus_accesses[" << index << "].ordinal = UINT64_C(" << bus.ordinal << ");\n"
        << "  transfer.stop.provenance.bus_accesses[" << index << "].kind = " << kind << ";\n"
        << "  transfer.stop.provenance.bus_accesses[" << index << "].address = UINT32_C(" << hex(bus.address.value, 8) << ");\n"
        << "  transfer.stop.provenance.bus_accesses[" << index << "].raw_byte_count = UINT8_C(" << bus.bytes.size() << ");\n"
        << "  transfer.stop.provenance.bus_accesses[" << index << "].region = GENESIS_REGION_RAW_CARTRIDGE_ROM;\n";
    for (std::size_t byte = 0; byte < bus.bytes.size(); ++byte)
      out << "  transfer.stop.provenance.bus_accesses[" << index << "].raw_bytes[" << byte << "] = UINT8_C(" << hex(bus.bytes[byte], 2) << ");\n";
  }
  out << "  return transfer;\n}\n";
  return out.str();
}

// A finite ADR-0009 target proof cannot be lowered as a subset dispatch.  If
// any target lacks an ordinary emitted block, this source-provenanced stop is
// returned before the control instruction can alter PC or create a call frame.
std::string build_genesis_tier1_indirect_pre_pc_stop(const InstructionProvenance &provenance) {
  std::ostringstream suffix;
  suffix << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
         << provenance.source.address.value;
  std::ostringstream out;
  out << "static GenesisControlTransfer genesis_tier1_indirect_stop_" << suffix.str()
      << "(GenesisRuntime *runtime) {\n"
      << "  GenesisInstructionProvenance source = {0};\n"
      << "  (void)runtime;\n"
      << "  source.cpu_variant = GENESIS_CPU_MC68000;\n"
      << "  source.source_address = UINT32_C(" << hex(provenance.source.address.value, 8) << ");\n"
      << "  source.image_offset = UINT64_C(" << provenance.source.image_offset.value << ");\n"
      << "  source.primary_bytes[0] = UINT8_C(" << hex(provenance.bytes[0], 2) << ");\n"
      << "  source.primary_bytes[1] = UINT8_C(" << hex(provenance.bytes[1], 2) << ");\n"
      << "  source.length = UINT32_C(" << provenance.length.value << ");\n"
      << "  return genesis_static_stop(GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET, "
         "GENESIS_DIAG_REACHED_UNRESOLVED_DIRECT_EDGE, &source, 0U, UINT32_C(0), "
         "GENESIS_ACCESS_LONG, GENESIS_ACCESS_READ);\n"
      << "}\n";
  return out.str();
}
} // namespace

namespace {
bool valid_c4_static_memory_fact(
    const M68kStaticMemoryFact &fact,
    const std::map<Address, const M68kDecodedInstruction *> &decoded) {
  const auto instruction = decoded.find(fact.operation.source.address.value);
  if (instruction == decoded.end() ||
      !same_provenance(fact.operation, instruction->second->provenance) ||
      !same_provenance(fact.source_provenance, instruction->second->provenance) ||
      !valid_program_address(fact.address) || !valid_access_width(fact.width) ||
      !valid_access_direction(fact.direction) || fact.width != instruction->second->size)
    return false;

  const M68kEffectiveAddress *expected_ea = nullptr;
  M68kMemoryAccessDirection expected_direction{};
  switch (instruction->second->kind) {
  case M68kInstructionKind::move:
    if (fact.role == M68kStaticMemoryFactRole::source_read) {
      expected_ea = &instruction->second->source_ea;
      expected_direction = M68kMemoryAccessDirection::read;
    } else if (fact.role == M68kStaticMemoryFactRole::destination_write) {
      expected_ea = &instruction->second->destination_ea;
      expected_direction = M68kMemoryAccessDirection::write;
    }
    break;
  case M68kInstructionKind::movea:
  case M68kInstructionKind::tst:
    if (fact.role == M68kStaticMemoryFactRole::source_read) {
      expected_ea = &instruction->second->source_ea;
      expected_direction = M68kMemoryAccessDirection::read;
    }
    break;
  case M68kInstructionKind::btst:
    // BTST's memory operand is its destination EA and is read-only; this
    // mirrors the identical btst destination_read handling the inline C4
    // static-memory-fact validator in emit_m68k_general_startup_runtime_c
    // already performs, keeping this preflight helper consistent with it.
    if (fact.role == M68kStaticMemoryFactRole::destination_read) {
      expected_ea = &instruction->second->destination_ea;
      expected_direction = M68kMemoryAccessDirection::read;
    }
    break;
  case M68kInstructionKind::cmpi:
    // SEG-007-T146 / SEG-007-T174 follow-up fix (reviewer-directed,
    // composed-hints regression): CMPI's immediate source is never a memory
    // fact; its destination is read-only (never written), forming the
    // CCR-only comparison result -- the identical destination_read-only
    // shape BTST's own case immediately above already uses, and the same
    // shape emit_m68k_general_startup_runtime_c's own inline re-verification
    // switch already recognizes for cmpi. This preflight switch previously
    // had no case for it at all, so any real foldable-EA CMPI fact reaching
    // it fell through to `default: break`, rejecting the entire C4 prefix.
    if (fact.role == M68kStaticMemoryFactRole::destination_read) {
      expected_ea = &instruction->second->destination_ea;
      expected_direction = M68kMemoryAccessDirection::read;
    }
    break;
  case M68kInstructionKind::clr:
  case M68kInstructionKind::andi:
  case M68kInstructionKind::ori:
  case M68kInstructionKind::eori:
    // SEG-007-T167: ORI/EORI reuse ANDI/CLR's destination-only retained-fact
    // convention -- their source is always an instruction-embedded immediate,
    // and a single destination_write fact is the independently re-verified
    // address/region authority for the whole read-modify-write memory
    // destination (both its read and its write).
    if (fact.role == M68kStaticMemoryFactRole::destination_write) {
      expected_ea = &instruction->second->destination_ea;
      expected_direction = M68kMemoryAccessDirection::write;
    }
    break;
  case M68kInstructionKind::logical_and:
  case M68kInstructionKind::logical_or:
  case M68kInstructionKind::eor:
    // SEG-007-T167: non-immediate AND/OR/EOR. A memory source operand is a
    // plain read (source_read); a memory read-modify-write destination
    // independently retains destination_read + destination_write, mirroring
    // ADD/SUBQ. The opposite operand is always a data register and owns no fact.
    if (fact.role == M68kStaticMemoryFactRole::source_read) {
      expected_ea = &instruction->second->source_ea;
      expected_direction = M68kMemoryAccessDirection::read;
    } else if (fact.role == M68kStaticMemoryFactRole::destination_read ||
               fact.role == M68kStaticMemoryFactRole::destination_write) {
      expected_ea = &instruction->second->destination_ea;
      expected_direction = fact.role == M68kStaticMemoryFactRole::destination_read
                                ? M68kMemoryAccessDirection::read
                                : M68kMemoryAccessDirection::write;
    }
    break;
  case M68kInstructionKind::sub:
  case M68kInstructionKind::add:
    // SEG-007-T170: SUB shares ADD's exact two-operand shape (`<ea>,Dn` or
    // `Dn,<ea>`); a memory source operand is a plain read (source_read), and
    // a memory read-modify-write destination independently retains
    // destination_read + destination_write, mirroring the AND/OR/EOR family
    // immediately above. SEG-007-T174 follow-up fix: ADD retains this exact
    // same fact shape (see machine/genesis/frontend.cpp's own `add` case,
    // which is byte-for-byte identical to its `sub` sibling immediately
    // below it), but this switch previously had no `add` case at all, so any
    // real ADD fact reaching here fell through to `default: break` --
    // `expected_ea` stayed null and the whole C4 preflight (and, because
    // `emit_m68k_general_startup_runtime_c` itself calls this preflight and
    // fails closed on `!preflight.valid`, the entire translation) was
    // rejected the instant a foldable-EA ADD fact was ever retained. Sharing
    // SUB's case is the required fix, not a new shape.
    if (fact.role == M68kStaticMemoryFactRole::source_read) {
      expected_ea = &instruction->second->source_ea;
      expected_direction = M68kMemoryAccessDirection::read;
    } else if (fact.role == M68kStaticMemoryFactRole::destination_read ||
               fact.role == M68kStaticMemoryFactRole::destination_write) {
      expected_ea = &instruction->second->destination_ea;
      expected_direction = fact.role == M68kStaticMemoryFactRole::destination_read
                                ? M68kMemoryAccessDirection::read
                                : M68kMemoryAccessDirection::write;
    }
    break;
  case M68kInstructionKind::adda:
  case M68kInstructionKind::suba:
  case M68kInstructionKind::cmpa:
  case M68kInstructionKind::cmp:
  // SEG-021-T018: MOVE <ea>,SR / MOVE <ea>,CCR read their (word) source only.
  case M68kInstructionKind::move_to_sr:
  case M68kInstructionKind::move_to_ccr:
  case M68kInstructionKind::chk:  // SEG-021-T019: CHK.W reads its word bound only (Dn is a register)
    // SEG-007-T174 follow-up fix: ADDA/SUBA/CMPA's fixed An (or, for CMPA,
    // CCR-only) destination carries no memory fact at all -- only the source
    // read is ever retained (see machine/genesis/frontend.cpp's shared
    // `cmpa`/`adda`/`suba` case, which retains exactly one source_read fact
    // for all three). This switch previously had no case for any of the
    // three, so the identical `default: break` / null-`expected_ea` / whole-
    // prefix-rejection defect described in the `add` case above applied here
    // too, for every one of these three kinds, the instant a foldable-EA
    // source fact was ever retained for any of them.
    //
    // SEG-007-T216: plain `CMP <ea>,Dn` shares this exact source-read-only
    // shape (its destination is always a data register, never a memory
    // fact) -- see machine/genesis/frontend.cpp's new `cmp` retain_fact case
    // this task added, which retains exactly one source_read fact mirroring
    // `cmpa`/`adda`/`suba` verbatim. This switch previously had no case for
    // plain `cmp` either, so the identical `default: break` / null-
    // `expected_ea` / whole-prefix-rejection defect applied to it too.
    if (fact.role == M68kStaticMemoryFactRole::source_read) {
      expected_ea = &instruction->second->source_ea;
      expected_direction = M68kMemoryAccessDirection::read;
    }
    break;
  case M68kInstructionKind::subi:
    // SEG-007-T170: SUBI's source is always the instruction-embedded
    // immediate (never a memory operand); its destination is a full RMW
    // operand, mirroring SUBQ's own destination_read/destination_write shape
    // immediately below.
    if (fact.role == M68kStaticMemoryFactRole::destination_read ||
        fact.role == M68kStaticMemoryFactRole::destination_write) {
      expected_ea = &instruction->second->destination_ea;
      expected_direction = fact.role == M68kStaticMemoryFactRole::destination_read
                                ? M68kMemoryAccessDirection::read
                                : M68kMemoryAccessDirection::write;
    }
    break;
  case M68kInstructionKind::subq:
  case M68kInstructionKind::addq:
  case M68kInstructionKind::addi:
    // SEG-007-T165: SUBQ's destination is a full RMW operand exactly like
    // ADD's (see machine/genesis/frontend.cpp's own retain_fact call for
    // subq), so both its destination_read and destination_write roles are
    // recognized here. SEG-007-T174 follow-up fix: `add`/`adda`/`suba` now
    // have their own cases above (see their case block's own comment).
    // SEG-007-T174 second follow-up fix (reviewer-directed, composed-hints
    // regression): ADDQ shares this identical destination-only RMW shape
    // byte-for-byte (its own source is likewise always the quick immediate)
    // but had no case here either, mirroring the exact same
    // machine/genesis/frontend.cpp retain_fact gap this same commit fixes --
    // any real foldable-EA ADDQ fact reaching this preflight without this
    // case would fall through to `default: break`, rejecting the entire C4
    // prefix, not merely that one instruction.
    if (fact.role == M68kStaticMemoryFactRole::destination_read ||
        fact.role == M68kStaticMemoryFactRole::destination_write) {
      expected_ea = &instruction->second->destination_ea;
      expected_direction = fact.role == M68kStaticMemoryFactRole::destination_read
                                ? M68kMemoryAccessDirection::read
                                : M68kMemoryAccessDirection::write;
    }
    break;
  // SEG-021-T009: the memory-word shift/rotate forms are one-address RMW operations with the same
  // destination_read/destination_write shape as NOT (the register form never carries a fact).
  case M68kInstructionKind::shift_rotate:
  // SEG-021-T014: NEG/NEGX are one-address RMW operands with NOT's fact shape.
  case M68kInstructionKind::negate_word:
  case M68kInstructionKind::negate_extended:
  case M68kInstructionKind::negate_decimal:
  case M68kInstructionKind::test_and_set:  // SEG-021-T016: TAS is a byte one-address RMW like NOT
  case M68kInstructionKind::set_conditional:  // SEG-021-T016: memory Scc reads then writes its byte destination
  case M68kInstructionKind::move_from_sr:  // SEG-021-T018: a memory MOVE from SR destination is read then written
  case M68kInstructionKind::not_operand:
    // SEG-007-T168: NOT has no second operand at all (unlike SUBQ's
    // quick-immediate source); its sole destination is a full RMW operand,
    // mirroring SUBQ's own destination_read/destination_write shape
    // immediately above.
    if (fact.role == M68kStaticMemoryFactRole::destination_read ||
        fact.role == M68kStaticMemoryFactRole::destination_write) {
      expected_ea = &instruction->second->destination_ea;
      expected_direction = fact.role == M68kStaticMemoryFactRole::destination_read
                                ? M68kMemoryAccessDirection::read
                                : M68kMemoryAccessDirection::write;
    }
    break;
  case M68kInstructionKind::bchg:
  case M68kInstructionKind::bclr:
  case M68kInstructionKind::bset:
    // SEG-007-T209: unlike BTST (destination_read only, above), BCHG/BCLR/
    // BSET always read-modify-write their destination -- the identical
    // SUBQ/NOT destination_read/destination_write shape immediately above.
    if (fact.role == M68kStaticMemoryFactRole::destination_read ||
        fact.role == M68kStaticMemoryFactRole::destination_write) {
      expected_ea = &instruction->second->destination_ea;
      expected_direction = fact.role == M68kStaticMemoryFactRole::destination_read
                                ? M68kMemoryAccessDirection::read
                                : M68kMemoryAccessDirection::write;
    }
    break;
  default: break;
  }
  if (expected_ea == nullptr || !m68k_is_statically_foldable_control_ea(*expected_ea) ||
      fact.direction != expected_direction ||
      fact.address.value != m68k_canonical_ea_address(*expected_ea))
    return false;

  const bool is_ram = m68k_startup_ram_range_in_range(
      fact.address.value, static_cast<std::uint32_t>(fact.width));
  if (is_ram) return fact.region == M68kAbsoluteOperandRegion::synthetic_work_ram;
  if (fact.region == M68kAbsoluteOperandRegion::raw_cartridge_rom)
    return fact.direction == M68kMemoryAccessDirection::read;
  const auto routed = m68k_route_genesis_device_access(
      M68kMemoryAccessRequest{fact.address, fact.width, fact.direction, fact.source_provenance});
  if (fact.region == M68kAbsoluteOperandRegion::controller_io)
    return fact.direction == M68kMemoryAccessDirection::read &&
           std::holds_alternative<M68kControllerIoResult>(routed);
  if (fact.region == M68kAbsoluteOperandRegion::vdp) {
    // SEG-007-T113: the VDP window is routable in both directions -- a WORD
    // read (SEG-007-T090) and a WORD/LONG store (this task) -- each verified
    // through the exact shared routing gate that produced the fact.
    if (fact.direction == M68kMemoryAccessDirection::read)
      return std::holds_alternative<M68kVdpRoutedRead>(routed);
    return fact.direction == M68kMemoryAccessDirection::write &&
           std::holds_alternative<M68kVdpRoutedWrite>(routed);
  }
  if (fact.region == M68kAbsoluteOperandRegion::routed_device) {
    // SEG-007-T115: a retained Z80 bus-arbitration / Z80 program-RAM / PSG
    // routed operand is valid only if the exact shared routing gate that
    // produced it still returns the generalized routed-device marker for the
    // same direction; nothing else is a valid routed_device fact.
    const auto *routed_access = std::get_if<M68kDeviceRoutedAccess>(&routed);
    return routed_access != nullptr && routed_access->direction == fact.direction;
  }
  return false;
}

// SEG-007-T075: structurally parallel to valid_c4_static_memory_fact above --
// independently re-derives every one of
// docs/architecture/c4-movem-adjacent-lea-constant-propagation-contract.md's
// conditions 1-6 from the decoded/lifted instruction stream, the block list,
// and the mapping-claim list, never merely trusting the discovery-time
// record for the producer/consumer identity, the shared register, the
// resolved base, the EA shape, the adjacency, or the mapping-claim
// containment. Consistent with valid_c4_static_memory_fact's own precedent
// (which never re-reads program.image.bytes for a raw_cartridge_rom fact's
// immutable_value) and with the fact that neither caller of this function is
// ever given a ROM image handle, the per-slot resolved literal values
// themselves are trusted from the discovery-time record.
bool valid_c4_movem_adjacent_lea_fact(
    const M68kMovemAdjacentLeaFact &fact,
    const std::map<Address, const M68kDecodedInstruction *> &decoded,
    const std::vector<M68kStaticBlock> &static_blocks,
    const std::vector<M68kStaticEdge> &static_edges,
    const std::vector<MappingClaim> &mapping_claims) {
  const auto producer_found = decoded.find(fact.producer.source.address.value);
  const auto consumer_found = decoded.find(fact.consumer.source.address.value);
  if (producer_found == decoded.end() || consumer_found == decoded.end() ||
      !same_provenance(fact.producer, producer_found->second->provenance) ||
      !same_provenance(fact.consumer, consumer_found->second->provenance))
    return false;
  const auto &producer = *producer_found->second;
  const auto &consumer = *consumer_found->second;
  // Condition 3.
  // SEG-007-T075 fix: re-derive resolved_base the exact same way the
  // discovery-time pass now does (see its own comment) -- LEA's own raw,
  // unmasked producer.source_ea.absolute_address, the literal value its own
  // C4 emission actually writes into the address register, never
  // m68k_canonical_ea_address's masked value. Re-deriving with the
  // same formula the discovery pass uses keeps this an independent
  // re-verification of the *fact*, not merely a self-consistent restatement
  // of a wrong formula: any accepted fact's resolved_base is provably
  // exactly what LEA's own statement assigns, for all three foldable EA
  // kinds (absolute_word, absolute_long, pc_disp16).
  if (producer.kind != M68kInstructionKind::lea || !m68k_is_statically_foldable_control_ea(producer.source_ea) ||
      fact.address_register != producer.destination_ea.reg ||
      fact.resolved_base != producer.source_ea.absolute_address)
    return false;
  // Condition 4.
  if (consumer.kind != M68kInstructionKind::movem ||
      consumer.movem_direction != M68kMovemDirection::memory_to_registers ||
      (consumer.source_ea.mode != M68kEaMode::address_indirect &&
       consumer.source_ea.mode != M68kEaMode::address_postinc) ||
      consumer.source_ea.reg != fact.address_register)
    return false;
  // Conditions 1, 2, 5: exact architectural adjacency and unique retained
  // ownership.  A synthesized static partition must not change this fact:
  // accept either adjacent positions in one block, or the terminal/entry
  // positions of two blocks joined by the sole incoming static fallthrough
  // relation.  Requiring that unique incoming relation rejects direct-branch
  // joins and ambiguous predecessors rather than inventing a producer fact
  // for an entry that can be reached with some other live address-register
  // value.
  if (static_cast<std::uint64_t>(producer.provenance.source.address.value) +
          producer.provenance.length.value !=
      consumer.provenance.source.address.value)
    return false;
  const M68kStaticBlock *producer_block = nullptr;
  const M68kStaticBlock *consumer_block = nullptr;
  std::size_t producer_index = 0U;
  std::size_t consumer_index = 0U;
  for (const auto &block : static_blocks) {
    for (std::size_t index = 0; index < block.instructions.size(); ++index) {
      if (same_provenance(block.instructions[index], fact.producer)) {
        if (producer_block != nullptr) return false;
        producer_block = &block;
        producer_index = index;
      }
      if (same_provenance(block.instructions[index], fact.consumer)) {
        if (consumer_block != nullptr) return false;
        consumer_block = &block;
        consumer_index = index;
      }
    }
  }
  if (producer_block == nullptr || consumer_block == nullptr) return false;
  if (producer_block == consumer_block) {
    if (producer_index + 1U != consumer_index) return false;
  } else {
    if (producer_index + 1U != producer_block->instructions.size() || consumer_index != 0U ||
        consumer_block->id.entry.value != consumer.provenance.source.address.value)
      return false;
    const M68kStaticEdge *incoming = nullptr;
    for (const auto &edge : static_edges) {
      if (edge.target.space != TargetAddressSpace::m68k_program ||
          edge.target.value != consumer_block->id.entry.value)
        continue;
      if (incoming != nullptr) return false;
      incoming = &edge;
    }
    if (incoming == nullptr ||
        (incoming->kind != M68kStaticEdgeKind::fallthrough &&
         incoming->kind != M68kStaticEdgeKind::fallthrough_continuation) ||
        !same_provenance(incoming->source_instruction, fact.producer))
      return false;
  }
  // Condition 6: whole-transfer single-claim containment.
  const auto order = m68k_movem_transfer_order(consumer.movem_register_mask, M68kMovemTransferOrder::ascending);
  if (fact.transfer_values.size() != order.size()) return false;
  const auto mapped = claims(mapping_claims, fact.resolved_base);
  if (mapped.size() != 1U) return false;
  const auto &claim = *mapped.front();
  const auto width = static_cast<std::uint32_t>(consumer.size);
  const auto span = static_cast<std::uint64_t>(order.size()) * width;
  if (static_cast<std::uint64_t>(fact.resolved_base) + span > claim.target_end.value) return false;
  return true;
}

// SEG-007-T077: independently re-derives every invariant the producer pass
// above checked, from the claim/bounds fields alone -- never from
// program.mapping_claims's caller-supplied ordering or index, and never by
// trusting resolved_bytes' own content (only its length, matching
// valid_c4_movem_adjacent_lea_fact's own "never re-read from a source image"
// precedent for M68kMovemAdjacentLeaFact.transfer_values). A fact whose
// claim.name/target_begin/target_end/image_begin/image_end does not exactly
// match a claim actually present in `mapping_claims` is rejected: this is
// the sole membership proof, since C4 has no image handle to re-derive
// mapping/bounds from first principles.
bool valid_c4_owned_cartridge_region_fact(const M68kOwnedCartridgeRegionFact &fact,
                                           const std::vector<MappingClaim> &mapping_claims) {
  const auto &claim = fact.claim;
  if (claim.target_begin.space != TargetAddressSpace::m68k_program ||
      claim.target_end.space != TargetAddressSpace::m68k_program ||
      claim.target_begin.value >= claim.target_end.value || claim.image_begin.value >= claim.image_end.value ||
      claim.image_end.value - claim.image_begin.value !=
          static_cast<std::uint64_t>(claim.target_end.value) - claim.target_begin.value ||
      claim.target_end.value > UINT32_C(0x00400000) ||
      fact.resolved_bytes.size() != claim.image_end.value - claim.image_begin.value)
    return false;
  return std::any_of(mapping_claims.begin(), mapping_claims.end(), [&](const MappingClaim &candidate) {
    return candidate.name == claim.name && candidate.target_begin.space == claim.target_begin.space &&
           candidate.target_begin.value == claim.target_begin.value &&
           candidate.target_end.space == claim.target_end.space &&
           candidate.target_end.value == claim.target_end.value &&
           candidate.image_begin.value == claim.image_begin.value &&
           candidate.image_end.value == claim.image_end.value;
  });
}

// The retirement seam consumes a C expression, not a partial scalar timing
// row.  Keep every dynamic row here so ordinary blocks and isolated AOT
// identities cannot silently diverge.
// SEG-021-T035: routed-only completeness probe for the immutable-ROM AOT boundary. Applies ONLY to
// kinds whose C lowering is gated on `runtime_routing` (DIVS.W/DIVU.W); returns false for every other
// kind so the shared non-routed probe remains their sole completeness authority. The context mirrors
// `emit_immutable_rom_aot_body`'s routed setup (Genesis runtime emitter, runtime object, routed operand
// access); an EA the lowering cannot express yields an empty body and is rejected.
bool immutable_rom_aot_routed_only_emission_is_complete(const M68kIrOperation &operation) {
  if (operation.kind != M68kIrKind::divide_signed_word && operation.kind != M68kIrKind::divide_unsigned_word)
    return false;
  if (m68k_operation_effect(operation).pc == M68kPcEffectKind::none) return false;
  GenesisM68kEmissionContext memory{};
  memory.program_counter = "pc";
  memory.address_registers = "runtime->a";
  memory.user_stack_pointer = "runtime->usp";
  memory.runtime_routing = true;
  memory.runtime_object = "runtime";
  memory.test_operand_access = M68kOperandAccess::runtime_routed;
  return !emit_m68k_operation_c(operation, "runtime->d", "runtime->sr", "  ", &memory).empty();
}

std::optional<std::string> m68k_retirement_cycle_expression(const M68kIrOperation &operation) {
  if (const auto cycles = m68k_instruction_cycles(operation))
    return "UINT32_C(" + std::to_string(*cycles) + ")";
  switch (operation.kind) {
  case M68kIrKind::general_branch:
    if (operation.condition != M68kCondition::always)
      return "m68k_branch_taken ? UINT32_C(10) : UINT32_C(" +
             std::to_string(operation.size == M68kMemoryAccessWidth::byte ? 8U : 12U) + ")";
    return std::nullopt;
  case M68kIrKind::dbcc_loop:
    return "m68k_dbcc_condition_true ? UINT32_C(12) : (m68k_dbcc_took_branch ? UINT32_C(10) : UINT32_C(14))";
  case M68kIrKind::set_conditional:
    // SEG-021-T016: Table 8-6 Scc Dn row is 4 (condition false) / 6 (true); memory rows are static (timing.cpp).
    if (operation.destination_ea.mode == M68kEaMode::data_register)
      return "m68k_scc_true ? UINT32_C(6) : UINT32_C(4)";
    return std::nullopt;
  case M68kIrKind::shift_rotate_register:
    return "UINT32_C(" + std::to_string(operation.size == M68kMemoryAccessWidth::long_word ? 8U : 6U) +
           ") + UINT32_C(2) * m68k_shift_effective_count";
  case M68kIrKind::multiply_unsigned_word:
  case M68kIrKind::multiply_signed_word: {
    const auto ea = m68k_effective_address_cycles(operation.source_ea, M68kMemoryAccessWidth::word);
    if (!ea) return std::nullopt;
    return std::string(operation.kind == M68kIrKind::multiply_unsigned_word
                           ? "genesis_m68k_mulu_word_cycles(m68k_timing_mul_source)"
                           : "genesis_m68k_muls_word_cycles(m68k_timing_mul_source)") +
           " + UINT32_C(" + std::to_string(*ea) + ")";
  }
  default: return std::nullopt;
  }
}

std::optional<std::map<Address, const FrontendAnalysis::ImmutableRomAotEntry *>>
validated_immutable_rom_aot_entries(const FrontendAnalysis &analysis) {
  std::map<Address, const FrontendAnalysis::ImmutableRomAotEntry *> result;
  // SEG-007-T246: the same conservative, never-unsound proxy
  // `populate_immutable_rom_aot_entries` (platforms/genesis/machine/src/frontend.cpp)
  // already used to decide RTS admission -- this codegen-side re-check must
  // agree with that analysis-time admission decision using the same
  // criterion, computed from the same `FrontendAnalysis` shape.
  const bool return_target_authority_available = !analysis.static_frames.empty();
  for (const auto &entry : analysis.immutable_rom_aot_entries) {
    const auto address = entry.decoded.provenance.source.address.value;
    // A candidate that analysis has retained as independently executable must
    // also have a complete shared retirement expression.  Dropping it here
    // would make the analysis/codegen boundary silently disagree and leave a
    // PC-keyed identity absent from generated dispatch.
    if (!m68k_retirement_cycle_expression(entry.operation)) return std::nullopt;
    const auto *selected = select_unique_affine_mapping(analysis.mapping_claims, entry.decoded.provenance);
    // SEG-021-T027: `m68k_operation_has_complete_c_emission`'s generic probe
    // requires `m68k_operation_effect(...).pc != M68kPcEffectKind::none`,
    // which the runtime-owned brief PC-indexed indirect JMP/JSR genuinely
    // never sets (ADR-0009: its target is runtime-only). That shape
    // nonetheless has a complete existing emission owner --
    // `m68k_operation_is_runtime_owned_indirect_jump` (platforms/genesis/
    // machine/include/.../frontend.hpp) is the narrow, explicit bypass for
    // exactly this shape; every other kind still requires the unmodified
    // `has_complete_c_emission` probe. SEG-021-T033: the same signal also
    // covers pure register-indirect `(An)` JMP/JSR.
    // SEG-021-T035: DIVS.W/DIVU.W emission exists only under runtime routing (the ADR-0037 vector-5
    // raise needs the live runtime object), so the shared non-routed probe is always empty for them.
    // Prove their completeness under the routed context `emit_immutable_rom_aot_body` actually uses;
    // scoped to exactly these routed-only kinds so no other kind's probe result changes.
    const bool has_complete_emission = m68k_operation_has_complete_c_emission(entry.operation) ||
                                        m68k_operation_is_runtime_owned_indirect_jump(entry.operation) ||
                                        immutable_rom_aot_routed_only_emission_is_complete(entry.operation);
    if (!m68k_operation_is_immutable_rom_aot_safe(entry.operation, return_target_authority_available) ||
        !has_complete_emission ||
        !independently_decoded_and_lifted(entry.decoded, entry.operation) || selected == nullptr ||
        selected->name != "raw_cartridge_rom" || selected->name != entry.source_mapping.name ||
        selected->target_begin.value != entry.source_mapping.target_begin.value ||
        selected->target_end.value != entry.source_mapping.target_end.value ||
        selected->image_begin.value != entry.source_mapping.image_begin.value ||
        selected->image_end.value != entry.source_mapping.image_end.value)
      return std::nullopt;
    const auto [found, inserted] = result.emplace(address, &entry);
    if (!inserted && (!same_decoded(found->second->decoded, entry.decoded) ||
                      !same_ir(found->second->operation, entry.operation)))
      return std::nullopt;
  }
  return result;
}

// Enumerate only PCs whose value is fixed by this isolated operation. Dynamic
// control already has a stronger runtime membership owner (currently RTS), so
// it deliberately contributes no exact obligation here. This consumes the
// shared CPU effect record rather than reconstructing instruction semantics.
std::optional<std::vector<Address>> immutable_rom_aot_exact_pc_obligations(
    const FrontendAnalysis::ImmutableRomAotEntry &entry) {
  const auto &operation = entry.operation;
  const auto effect = m68k_operation_effect(operation);
  std::vector<Address> result;
  switch (effect.pc) {
  case M68kPcEffectKind::advance:
    result.push_back(static_cast<Address>(entry.decoded.provenance.source.address.value + effect.pc_delta));
    break;
  case M68kPcEffectKind::direct_target:
    result.push_back(effect.direct_target);
    if ((operation.kind == M68kIrKind::general_branch && operation.condition != M68kCondition::always) ||
        operation.kind == M68kIrKind::dbcc_loop)
      result.push_back(static_cast<Address>(entry.decoded.provenance.source.address.value +
                                            entry.decoded.provenance.length.value));
    break;
  case M68kPcEffectKind::observed_stack_return:
    // emit_m68k_operation_c validates the observed stack PC against the
    // existing whole-program runtime-return authority before assigning it.
    break;
  case M68kPcEffectKind::none:
    // SEG-021-T027: the only admitted AOT candidate that ever reaches this
    // function with `effect.pc == none` is the runtime-owned indirect
    // JMP/JSR (brief PC-indexed, and since SEG-021-T033 pure `(An)`) (`m68k_operation_is_immutable_rom_aot_
    // safe` rejects every other kind before an entry can ever reach
    // `aot_entries` with this effect shape). Exactly like `observed_stack_
    // return` above, its target already has a stronger existing runtime
    // membership owner (`m68k_indirect_target_member` against the caller's
    // final compiled-address authority, checked inside `emit_m68k_
    // operation_c`'s own unchanged dynamic-indirect lowering) -- it
    // deliberately contributes no exact-PC obligation here, never a second
    // target-proof mechanism. Any other kind reaching `none` would be a
    // genuine analysis/codegen boundary inconsistency and must still fail
    // loud.
    if (operation.kind == M68kIrKind::jump_general || operation.kind == M68kIrKind::call_general) break;
    return std::nullopt;
  case M68kPcEffectKind::observed_exception_return:
    // SEG-021-T018: like `observed_stack_return`, the restored PC is a runtime fact popped from the exception
    // frame by the platform's exception-return routine (the M68K-owned core); it continues through the ordinary
    // dispatcher, whose fail-closed stop owns any PC outside the emitted set. No exact-PC obligation.
    break;
  case M68kPcEffectKind::exception_entry:
    // SEG-021-T019: the PC becomes the build-time-resolved vector handler selected by the platform's raise (a
    // dispatch root of the program), or the run stops fail-closed. No exact-PC obligation.
    break;
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

// Compare exact AOT PC assignments with the caller's final dispatch/frontier
// authority. This is deliberately not a graph: C3 and C4 supply their existing
// represented-address sets, and this helper only identifies the post-retirement
// mismatch each producer must turn into a truthful typed stop.
std::optional<std::map<Address, std::vector<Address>>> immutable_rom_aot_unrepresented_exact_pcs(
    const std::map<Address, const FrontendAnalysis::ImmutableRomAotEntry *> &entries,
    const std::set<Address> &represented_exact_pcs) {
  std::map<Address, std::vector<Address>> result;
  for (const auto &[address, entry] : entries) {
    const auto obligations = immutable_rom_aot_exact_pc_obligations(*entry);
    if (!obligations) return std::nullopt;
    for (const auto target : *obligations)
      if (!represented_exact_pcs.contains(target)) result[address].push_back(target);
    if (result.contains(address) &&
        (entry->source_mapping.name.size() > genesis_frontier_max_name_length ||
         entry->decoded.raw_bytes.size() > genesis_frontier_max_raw_bytes))
      return std::nullopt;
  }
  return result;
}

std::string emit_immutable_rom_aot_body(const FrontendAnalysis::ImmutableRomAotEntry &entry,
                                          const std::vector<std::uint32_t> &runtime_return_targets,
                                          const std::vector<std::uint32_t> &unrepresented_exact_pcs,
                                          const std::vector<std::uint32_t> &indirect_candidate_targets,
                                          bool use_shared_compiled_entry_lookup,
                                          std::string_view owner_entry_label,
                                          const AotBodyFactoring &factoring) {
  const auto address = entry.decoded.provenance.source.address.value;
  const auto cycle_expression = m68k_retirement_cycle_expression(entry.operation);
  if (!cycle_expression)
    return "/* translation rejected: immutable-ROM AOT timing is unaccounted */\n";
  std::ostringstream out;
  // SEG-022-T008: an owner-grouped entry is a labelled block inside a shared owner function; the
  // block body (locals, lowering, retirement, every return) is exactly the standalone function body.
  if (!factoring.source_symbol.empty()) {
    // Inner body only (the caller owns the header and the closing brace).
  } else if (factoring.bare_block)
    out << "{\n";
  else if (owner_entry_label.empty())
    out << "static GenesisControlTransfer genesis_aot_" << std::uppercase << std::hex
        << std::setw(8) << std::setfill('0') << address << "(GenesisRuntime *runtime) {\n";
  else
    out << owner_entry_label << ": {\n";
  out << "  uint32_t pc = runtime->pc;\n";
  if (entry.operation.kind == M68kIrKind::dbcc_loop)
    out << "  uint8_t m68k_dbcc_took_branch = 0U;\n";
  const bool scc_dynamic_timing = entry.operation.kind == M68kIrKind::set_conditional &&
                                  entry.operation.destination_ea.mode == M68kEaMode::data_register;
  if (scc_dynamic_timing) out << "  uint8_t m68k_scc_true = 0U;\n";
  if (entry.operation.kind == M68kIrKind::multiply_signed_word ||
      entry.operation.kind == M68kIrKind::multiply_unsigned_word)
    out << "  uint16_t m68k_timing_mul_source = UINT16_C(0);\n";
  GenesisM68kEmissionContext memory{};
  memory.execution_history_hooks = g_execution_history_hooks;
  memory.program_counter = "pc";
  memory.address_registers = "runtime->a";
  memory.user_stack_pointer = "runtime->usp";
  if (entry.operation.kind == M68kIrKind::dbcc_loop)
    memory.timing_dbcc_taken = "m68k_dbcc_took_branch";
  if (scc_dynamic_timing) memory.timing_scc_true = "m68k_scc_true";
  // SEG-007-T245: every immutable-ROM AOT candidate that
  // `m68k_operation_is_immutable_rom_aot_safe` admits is, by construction,
  // safe to route through the runtime device/memory gate -- register-only
  // operations never consult `runtime_routing` at all, and the narrow
  // register-indirect `write_move` source-read family this task adds
  // requires it (see that predicate's own doc comment). `emit_m68k_
  // operation_c` derives `runtime_source`/`runtime_provenance_helper`
  // generically from `entry.operation`'s own provenance, so no further
  // per-instruction wiring is needed here.
  memory.runtime_routing = true;
  memory.runtime_object = "runtime";
  // SEG-021-T027: the runtime-owned brief PC-indexed indirect JMP/JSR
  // (`(d8,PC,Xn)`, word index only) reuses the exact existing
  // `jump_general`/`call_general` dynamic-indirect branch in
  // `emit_m68k_operation_c` (libs/codegen/c11/src/m68k.cpp) verbatim -- the
  // same runtime EA computation and the same `m68k_indirect_target_member`
  // membership check the ordinary C4 route already performs for this exact
  // shape (ADR-0009). That branch only ever consults
  // `indirect_candidate_targets` when its own EA-shape guard already
  // matches (`pc_index8`/`(An)`-indirect), so supplying the caller's
  // already-computed final compiled-address authority here is harmless for
  // every other admitted operation kind, which never reaches that branch at
  // all.
  memory.indirect_candidate_targets = indirect_candidate_targets;
  // SEG-022-T006: the sharded/C4 caller supplies no site-local copy; membership queries the one final
  // compiled-entry table.
  if (use_shared_compiled_entry_lookup) memory.compiled_entry_lookup_symbol = "genesis_compiled_entry_lookup";
  // SEG-021-T005: an isolated AOT candidate has no whole-program absolute-
  // operand fact; absolute and d16(PC) source reads take the runtime-routed
  // read (never a folded constant).
  memory.test_operand_access = M68kOperandAccess::runtime_routed;
  if (entry.operation.kind == M68kIrKind::multiply_signed_word ||
      entry.operation.kind == M68kIrKind::multiply_unsigned_word)
    memory.timing_mul_source = "m68k_timing_mul_source";
  // SEG-007-T246: the caller supplies the SAME already-computed whole-
  // program `runtime_return_target_set` (ADR-0011 Decision §1) every
  // ordinary CFG-rooted `return_from_subroutine` emission already uses.
  // Harmless for every other admitted operation kind, since none of their
  // lowering cases ever consult `runtime_return_targets`; consulted only by
  // `return_from_subroutine`'s existing, unchanged lowering.
  memory.runtime_return_targets = runtime_return_targets;
  // SEG-007-T246: a call's own pushed continuation is always this exact
  // candidate's own provenance (source address + instruction length) --
  // never an external/whole-program fact, unlike `runtime_return_targets`
  // above. Harmless for every non-call operation kind (none of their
  // lowering cases ever consult `memory.continuation`); consulted only by
  // the existing, unchanged `call_general`/`bsr_call` lowering.
  memory.continuation = address + entry.decoded.provenance.length.value;
  memory.factored_route_failure = factoring.route_failure;
  memory.runtime_source_symbol = factoring.source_symbol;
  out << emit_m68k_operation_c(entry.operation, "runtime->d", "runtime->sr", "  ", &memory)
      << "  runtime->pc = pc;\n";
  if (!unrepresented_exact_pcs.empty()) {
    const auto &claim = entry.source_mapping;
    out << "  if (";
    for (std::size_t index = 0; index < unrepresented_exact_pcs.size(); ++index) {
      if (index != 0U) out << " || ";
      out << "runtime->pc == UINT32_C(" << hex(unrepresented_exact_pcs[index], 8) << ")";
    }
    out << ") {\n"
        << "    GenesisInstructionProvenance source = {0};\n"
        << "    source.cpu_variant = GENESIS_CPU_MC68000;\n"
        << "    source.source_address = UINT32_C(" << hex(address, 8) << ");\n"
        << "    source.image_offset = UINT64_C(" << std::dec
        << entry.decoded.provenance.source.image_offset.value << ");\n"
        << "    source.primary_bytes[0] = UINT8_C(" << hex(entry.decoded.provenance.bytes[0], 2) << ");\n"
        << "    source.primary_bytes[1] = UINT8_C(" << hex(entry.decoded.provenance.bytes[1], 2) << ");\n"
        << "    source.length = UINT32_C(" << entry.decoded.provenance.length.value << ");\n";
    out << "    GenesisControlTransfer frontier = {0};\n"
        << "    frontier.kind = GENESIS_STOP;\n"
        << "    frontier.stop.stop_class = GENESIS_STOP_KNOWN_BUT_UNEMITTED_TARGET;\n"
        << "    frontier.stop.diagnostic_category = GENESIS_DIAG_KNOWN_BUT_UNEMITTED_TARGET;\n"
        << "    frontier.stop.provenance.has_instruction_provenance = UINT8_C(1);\n"
        << "    frontier.stop.provenance.instruction = source;\n"
        << "    frontier.stop.provenance.mapping_claim_count = UINT8_C(1);\n";
    out << compact_mapping_claim_call("    ", "&frontier.stop.provenance", 0U, claim)
        << compact_fetch_access_call("    ", "&frontier.stop.provenance", "source.source_address", entry.decoded.raw_bytes);
    out << "    return " << retire_before_stop_call_open(
        address, entry.decoded.provenance.length.value, entry.operation.kind);
    out << *cycle_expression;
    out << ", runtime->pc, &frontier);\n"
        << "  }\n";
  }
  out << "  { const uint32_t m68k_retirement_pc = runtime->pc; GenesisControlTransfer retired = "
      << retire_call_open(address, entry.decoded.provenance.length.value, entry.operation.kind);
  out << *cycle_expression;
  out << ", runtime->pc);\n"
      << "    if (retired.kind != GENESIS_CONTINUE_AT_PC || retired.next_pc != m68k_retirement_pc) return retired;\n"
      << "    return retired;\n  }\n";
  if (factoring.source_symbol.empty()) out << "}\n";
  return out.str();
}
} // namespace

// SEG-007-T142 correction: one per-operation gap-shape classifier, shared
// verbatim by preflight_m68k_general_startup_c4's address-free advisory
// inventory below and each generated `genesis_c4_lowering_stop_<addr>`'s
// own dimension-literal selection in emit_m68k_general_startup_runtime_c
// (FrontendPartialProgram overload), so the advisory preflight inventory
// and the runtime stop dimension can never silently diverge.
struct M68kC4GapShape {
  M68kC4OperandRole operand_role{M68kC4OperandRole::operation};
  M68kMemoryAccessWidth width{M68kMemoryAccessWidth::word};
  M68kEaMode ea_class{M68kEaMode::data_register};
  M68kC4AutoUpdateClass auto_update{M68kC4AutoUpdateClass::none};
  M68kC4GapClass gap{};
  std::string_view predecessor;
};

M68kC4AutoUpdateClass m68k_c4_auto_update_class(M68kEaMode mode) {
  return mode == M68kEaMode::address_predec ? M68kC4AutoUpdateClass::predecrement :
         mode == M68kEaMode::address_postinc ? M68kC4AutoUpdateClass::postincrement :
                                                M68kC4AutoUpdateClass::none;
}

// The exhaustive set of M68kIrKind values C4 currently has a lowering body
// for.  Anything else is a `missing_dispatcher` gap -- this predicate is the
// single source of truth for that split; both the preflight inventory and
// the emitted-stop classifier below consume it, never a separate copy.
bool m68k_c4_represented_ir_kind(M68kIrKind kind) {
  switch (kind) {
  case M68kIrKind::write_moveq:
  case M68kIrKind::subtract_quick_long_d0:
  case M68kIrKind::subtract_quick:
  case M68kIrKind::load_effective_address:
  // SEG-021-T011: PEA is a represented C4 kind (no missing_dispatcher gap).
  // Like LEA immediately above, its source_ea is address-computation only --
  // it never reads memory content, so `classify_m68k_c4_gap_shapes` needs no
  // per-kind branch for it (same "represented, no further per-kind gap
  // handling" treatment as LEA: no absolute/pc-relative EA ever needs a
  // retained-fact check because no memory read occurs). The routed -(A7)
  // push itself is lowered through the atomic local-snapshot/deferred-commit
  // technique in emit_m68k_operation_c's push_effective_address case (see
  // that case's own comment), so a routed write failure leaves A7 and every
  // other register/PC exactly as they were before the instruction.
  case M68kIrKind::push_effective_address:
  case M68kIrKind::general_branch:
  case M68kIrKind::write_user_stack_pointer:
  // SEG-021-T018: MOVE USP,An and ANDI/ORI/EORI to CCR/SR carry no memory operand (no retained fact).
  case M68kIrKind::read_user_stack_pointer:
  case M68kIrKind::logical_immediate_to_ccr:
  case M68kIrKind::logical_immediate_to_sr:
  // SEG-021-T019: TRAP/TRAPV/RTR and the instruction-word exceptions carry no memory operand; CHK.W's word bound has
  // the MOVE to SR source gap shape below.
  case M68kIrKind::trap_exception:
  case M68kIrKind::trap_on_overflow:
  case M68kIrKind::check_bounds:
  case M68kIrKind::return_restore_condition_codes:
  case M68kIrKind::instruction_exception:
  case M68kIrKind::write_clr:
  // SEG-007-T168: NOT (`logical_not`) is a represented kind (no
  // missing_dispatcher gap) -- emit_m68k_operation_c has a full lowering
  // body for it, matching write_clr/add's own already-represented shape.
  case M68kIrKind::logical_not:
  case M68kIrKind::negate_word:
  // SEG-021-T014: NEGX shares NEG's one-address RMW lowering; ADDX/SUBX/CMPM are register-pair or
  // predecrement/postincrement-pair operations (always Dn or auto-updating operands, never a retained
  // fact) lowered by `m68k_emit_extended_pair` through the operation-local deferred address commit.
  case M68kIrKind::negate_extended:
  case M68kIrKind::add_extended:
  case M68kIrKind::subtract_extended:
  case M68kIrKind::compare_memory:
  // SEG-021-T015: ABCD/SBCD/NBCD reuse the ADDX/SUBX pair and NEG/NEGX RMW lowerings.
  case M68kIrKind::negate_decimal:
  case M68kIrKind::add_decimal:
  case M68kIrKind::subtract_decimal:
  // SEG-021-T016: EXG/MOVEP never carry a retained fact (register-only / d16(An)); Scc has CLR's and TAS has NOT's
  // gap shape below.
  case M68kIrKind::exchange_registers:
  case M68kIrKind::movep_transfer:
  case M68kIrKind::set_conditional:
  case M68kIrKind::test_and_set:
  case M68kIrKind::logical_and_immediate:
  case M68kIrKind::write_move:
  case M68kIrKind::write_movea:
  case M68kIrKind::test_operand:
  case M68kIrKind::jump_general:
  case M68kIrKind::call_general:
  case M68kIrKind::bsr_call:
  case M68kIrKind::movem_transfer:
  case M68kIrKind::return_from_subroutine:
  case M68kIrKind::return_from_exception:
  case M68kIrKind::add:
  case M68kIrKind::add_address:
  case M68kIrKind::subtract_address:
  case M68kIrKind::compare:
  case M68kIrKind::compare_immediate:
  case M68kIrKind::compare_address:
  case M68kIrKind::bit_test:
  // SEG-007-T209: BCHG/BCLR/BSET are represented C4 kinds (no
  // missing_dispatcher gap). They share the exact bit-manipulation emission
  // body in emit_m68k_operation_c with the already-represented BTST sibling
  // immediately above (`case M68kIrKind::bit_test: ... bit_set:` in
  // m68k.cpp), differing only in `M68kBitOperationKind` and in performing a
  // destination write BTST never does. Their per-kind gap shape is classified
  // in classify_m68k_c4_gap_shapes below, mirroring BTST's destination
  // resolver-fact/auto-update treatment but against the write-side fact
  // (this family reads AND writes its destination, matching SUBQ/SUBI's own
  // "read side threaded from the write fact" RMW shape rather than BTST's
  // read-only one).
  // SEG-021-T009: memory-word shift/rotate is a represented C4 kind: a one-address RMW lowered through
  // the shared routed read/write primitives (auto-updating destinations via the operation-local
  // deferred address-register commit); gap shapes are classified below like NOT's.
  case M68kIrKind::shift_rotate_memory:
  case M68kIrKind::bit_change:
  case M68kIrKind::bit_clear:
  case M68kIrKind::bit_set:
  case M68kIrKind::dbcc_loop:
  // SEG-007-T088: represented (no missing_dispatcher gap), like
  // write_user_stack_pointer above; its decoded source_ea is never one of
  // the three absolute/PC-relative "requires a static fact" modes (see
  // m68k_ea_move_to_sr_source's own doc comment), so no `check_fact`/
  // `auto_update`-gap branch is needed for it below, matching
  // return_from_subroutine's own identical "represented, no further
  // per-kind gap handling" treatment.
  case M68kIrKind::write_status_register:
  // SEG-007-T114: NOP is represented with no missing_dispatcher gap: it has
  // no operand, no EA, and no memory access at all, so no `check_fact`/
  // `auto_update`-gap branch is needed for it below -- the same "represented,
  // no further per-kind gap handling" treatment as return_from_subroutine.
  case M68kIrKind::no_operation:
  // SEG-007-T116: MOVE from SR is represented with no missing_dispatcher gap:
  // its decoded destination is restricted to data-register-direct (see
  // m68k_ea_move_from_sr_destination), never a mode that needs a resolver
  // fact / runtime-routed context, so no `check_fact`/`auto_update`-gap
  // branch is needed -- the same treatment as write_status_register above.
  case M68kIrKind::read_status_register:
  // SEG-007-T118: MOVE <ea>,CCR is represented with no missing_dispatcher gap:
  // its decoded source is restricted to data-register-direct (see
  // m68k_ea_move_to_ccr_source), never a mode that needs a resolver fact /
  // runtime-routed context, so no `check_fact`/`auto_update`-gap branch is
  // needed -- the same treatment as write_status_register above.
  case M68kIrKind::write_condition_codes:
  // SEG-007-T152: the register-count shift/rotate form is represented with no
  // missing_dispatcher gap. Its destination_ea is architecturally always
  // `data_register` -- the register/memory split in m68k_lift_operation
  // decides `shift_rotate_register` vs. `shift_rotate_memory` purely from
  // `instruction.destination_ea.mode == M68kEaMode::data_register`, so a
  // `shift_rotate_register` operation can never carry a destination needing a
  // resolver fact. Its source_ea is restricted to `immediate` (fixed shift
  // count) or `data_register` (count register) by the decoder/emitter (see
  // emit_m68k_operation_c's shift_rotate_register case), neither of which is
  // a mode `m68k_is_statically_foldable_control_ea` ever flags, so no
  // `check_fact`/`auto_update`-gap branch is needed below -- the same
  // "represented, no further per-kind gap handling" treatment as
  // write_condition_codes above.
  case M68kIrKind::shift_rotate_register:
  // SEG-007-T153: ADDQ (`add_quick`) is a represented C4 kind (no
  // missing_dispatcher gap). Its quick immediate source is embedded in the
  // instruction word (`M68kEaMode::immediate`, never a memory read); the
  // shared `add` / `add_immediate` / `add_quick` / `add_address` emission body
  // in emit_m68k_operation_c lowers every destination the MC68000 permits:
  // Dn/An direct, a non-auto-updating memory RMW destination
  // ((An)/(d16,An)/abs, same routed read+write + retained-fact treatment as
  // `add`), and an auto-updating destination ((An)+/-(An), via the add-family
  // deferred single-address-register commit path). An indexed ((d8,An,Xn))
  // destination is already declined by the shared decoder EA mask
  // (`m68k_ea_data_alterable` excludes brief-format index modes), so it never
  // reaches this classifier. Its per-kind branch in
  // classify_m68k_c4_gap_shapes mirrors `add`'s destination read+write
  // retained-fact rows for a foldable absolute work_ram destination.
  case M68kIrKind::add_quick:
  // ADDI has the same immediate-source, destination-RMW shape as ADDQ, but
  // its immediate is materialized from extension words by the typed decoder.
  // The shared add-family lowerer owns both that materialization and deferred
  // address-register commit for auto-updating destinations.
  case M68kIrKind::add_immediate:
  // SEG-007-T167: the C4 logical-family missing-dispatcher batch. AND/OR/EOR
  // (`<ea>,Dn` or `Dn,<ea>`) and ORI/EORI (`#imm,<ea>`) are decoded, lifted,
  // and share the existing logical emission body in emit_m68k_operation_c
  // (the same `case M68kIrKind::logical_and: ... exclusive_or_immediate:`
  // block that already lowers the represented sibling `logical_and_immediate`
  // / ANDI). One operand is always a data register; the other may be a
  // foldable-control memory EA whose retained read/write fact is checked in
  // classify_m68k_c4_gap_shapes below exactly as the add/compare families do.
  // An auto-updating (`(An)+`/`-(An)`) operand is declined cleanly via
  // M68kC4GapClass::requires_architecture_decision -- the logical family
  // carries no deferred-address-commit contract, matching its already-
  // represented sibling ANDI (the compare/SUB families gained theirs in SEG-021-T006).
  case M68kIrKind::logical_and:
  case M68kIrKind::logical_or:
  case M68kIrKind::logical_or_immediate:
  case M68kIrKind::exclusive_or:
  case M68kIrKind::exclusive_or_immediate:
  // SEG-007-T170: the C4 subtract-family missing-dispatcher batch. SUB
  // (`<ea>,Dn` or `Dn,<ea>`) and SUBI (`#imm,<ea>`) are decoded, lifted, and
  // already share the existing SUBA/SUBQ subtraction emission body in
  // emit_m68k_operation_c (the same `case M68kIrKind::subtract: ...
  // subtract_address:` block). SUB's `<ea>,Dn`/`Dn,<ea>` shape mirrors ADD's
  // own two-operand shape; SUBI's immediate source mirrors CMPI's shape but
  // with an RMW (not read-only) destination. Their retained read/write fact
  // and auto-update treatment is classified in classify_m68k_c4_gap_shapes
  // below, mirroring the add/compare/logical families.
  case M68kIrKind::subtract:
  case M68kIrKind::subtract_immediate:
  // SEG-007-T177: EXT.W/EXT.L (`sign_extend_word`/`sign_extend_long`) are
  // represented C4 kinds (no missing_dispatcher gap). Both are decoded with
  // their operand fixed to `M68kEaMode::data_register` (see the shared
  // `ext_w`/`ext_l` decode path in decode.cpp, which always constructs the
  // operand EA as `{M68kEaMode::data_register, reg, 0, 0, 0, 0}` -- EXT has
  // no memory destination on the base MC68000), exactly mirroring
  // `shift_rotate_register`'s own already-represented "destination_ea is
  // architecturally always data_register" treatment above. The existing
  // emission body in emit_m68k_operation_c already lowers both kinds
  // completely (materialized register read, sign-extend, register write, CCR
  // update). Because the destination can never be a mode requiring a
  // resolver fact or auto-update-gap branch, no `check_fact`/`auto_update`
  // branch is needed in classify_m68k_c4_gap_shapes below -- the same
  // "represented, no further per-kind gap handling" treatment as
  // write_condition_codes/shift_rotate_register above.
  case M68kIrKind::sign_extend_word:
  case M68kIrKind::sign_extend_long: return true;
  // SEG-007-T224: SWAP is a represented C4 kind. The shared C11 lowerer
  // already owns its complete Dn-only read/write and CCR update; like EXT it
  // has no memory EA, retained fact, or runtime-routing requirement.
  case M68kIrKind::write_swap: return true;
  // SEG-007-T220: MULS.W is a represented C4 kind (no missing_dispatcher
  // gap). Its destination is architecturally always
  // `{M68kEaMode::data_register, Dn}` (decode.cpp always constructs it that
  // way), exactly mirroring EXT.W/EXT.L's own "destination can never need a
  // resolver fact" treatment above; only its source EA needs the
  // check_fact/auto_update classification below, mirroring the
  // compare/compare_address family's own read-only-source shape.
  case M68kIrKind::multiply_signed_word: return true;
  // SEG-007-T222: MULU.W/DIVS.W/DIVU.W share MULS.W's exact "represented, no
  // missing_dispatcher gap, destination is always Dn" shape.
  case M68kIrKind::multiply_unsigned_word:
  case M68kIrKind::divide_signed_word:
  case M68kIrKind::divide_unsigned_word: return true;
  default: return false;
  }
}

bool m68k_c4_lacks_runtime_routing(const M68kIrOperation &operation, const std::vector<MappingClaim> &mapping_claims) {
  // C4 deliberately retains no per-slot facts for MOVEM.  Consequently a
  // statically addressed memory-to-register MOVEM source is routed at
  // runtime (the MOVEM lowering's documented rule), rather than folded.
  // The runtime has no ROM-read success route: a routed ROM read is an
  // internal-dispatch-inconsistency stop, not an executable MOVEM source.
  // Reduce the retained address immediately to the existing region class;
  // the resulting shape exposes only its normalized operation/EA fields and
  // never the address or mapping claim.
  if (operation.kind != M68kIrKind::movem_transfer ||
      operation.movem_direction != M68kMovemDirection::memory_to_registers ||
      !m68k_is_statically_foldable_control_ea(operation.source_ea))
    return false;
  return m68k_frontier_region_class_name(
             m68k_canonical_ea_address(operation.source_ea),
             static_cast<std::uint32_t>(operation.size), mapping_claims) ==
         "raw_cartridge_rom";
}

// Every gap row this one operation contributes -- zero, one, or (for a
// represented operation whose source and destination are independently
// checked, e.g. MOVE) up to two. Order matches preflight's own historical
// per-operation visitation order exactly, so a caller that wants a single
// representative shape for one instruction (the emitted stop's own
// dimension) may deterministically take the first row.
std::vector<M68kC4GapShape> classify_m68k_c4_gap_shapes(
    const M68kIrOperation &operation, const std::set<std::pair<Address, M68kStaticMemoryFactRole>> &facts,
    const std::vector<MappingClaim> &mapping_claims) {
  std::vector<M68kC4GapShape> rows;
  const auto add = [&](M68kC4OperandRole role, M68kEaMode ea, M68kC4AutoUpdateClass update, M68kC4GapClass gap,
                       std::string_view predecessor) {
    rows.push_back({role, operation.size, ea, update, gap, predecessor});
  };
  const auto check_fact = [&](const M68kEffectiveAddress &ea, M68kC4OperandRole role,
                               M68kStaticMemoryFactRole fact_role) {
    if (m68k_is_statically_foldable_control_ea(ea) &&
        !facts.contains({operation.provenance.source.address.value, fact_role}))
      add(role, ea.mode, m68k_c4_auto_update_class(ea.mode), M68kC4GapClass::missing_fact, "retain_fact");
  };
  if (!m68k_c4_represented_ir_kind(operation.kind)) {
    add(M68kC4OperandRole::operation, operation.source_ea.mode,
        m68k_c4_auto_update_class(operation.source_ea.mode), M68kC4GapClass::missing_dispatcher,
        "C4 block dispatcher");
    return rows;
  }
  if (m68k_c4_lacks_runtime_routing(operation, mapping_claims)) {
    add(M68kC4OperandRole::source, operation.source_ea.mode,
        m68k_c4_auto_update_class(operation.source_ea.mode), M68kC4GapClass::missing_routing,
        "genesis_route_access");
    return rows;
  }
  if (operation.kind == M68kIrKind::write_movea) {
    // SEG-007-T192: an auto-updating MOVEA source is now lowered by its own
    // deferred-address-commit path in emit_m68k_operation_c (see
    // docs/architecture/c4-move-predecrement-postincrement-commit-contract.md
    // and docs/architecture/c4-add-family-auto-update-commit-contract.md, whose
    // technique it directly reuses); it is no longer a
    // requires_architecture_decision gap. Unlike ADDA, MOVEA's destination
    // write never reads the destination register's own prior value, so there
    // is no same-register aliasing shape to decline either. A non-auto-update
    // foldable source still needs its retained fact.
    check_fact(operation.source_ea, M68kC4OperandRole::source, M68kStaticMemoryFactRole::source_read);
  } else if (operation.kind == M68kIrKind::write_move) {
    check_fact(operation.source_ea, M68kC4OperandRole::source, M68kStaticMemoryFactRole::source_read);
    check_fact(operation.destination_ea, M68kC4OperandRole::destination, M68kStaticMemoryFactRole::destination_write);
  } else if (operation.kind == M68kIrKind::add_quick) {
    // SEG-007-T153: ADDQ mirrors the `add` read-modify-write destination-fact
    // shape below. Its source is the instruction-embedded quick immediate
    // (never a memory read, never statically foldable), so only the
    // destination needs consideration. Dn/An destinations need nothing. A
    // non-auto-updating memory destination ((An)/(d16,An)/abs) needs its
    // retained read+write fact exactly like `add`. Auto-updating destinations
    // ((An)+/-(An)) are fully lowered by the add-family deferred-address-commit
    // path (emit_m68k_operation_c), so they produce no gap row. An indexed
    // ((d8,An,Xn)) destination is never reachable here: the shared decoder EA
    // mask `m68k_ea_data_alterable` excludes brief-format index modes for the
    // whole data-alterable family, so the decoder already declines it as
    // `unsupported_instruction_form` before any prefix is retained.
    if (operation.destination_ea.mode != M68kEaMode::data_register &&
        operation.destination_ea.mode != M68kEaMode::address_register &&
        m68k_c4_auto_update_class(operation.destination_ea.mode) == M68kC4AutoUpdateClass::none) {
      check_fact(operation.destination_ea, M68kC4OperandRole::destination, M68kStaticMemoryFactRole::destination_read);
      check_fact(operation.destination_ea, M68kC4OperandRole::destination, M68kStaticMemoryFactRole::destination_write);
    }
  } else if (operation.kind == M68kIrKind::add_immediate) {
    // ADDI's extension-word immediate is not a memory access. Its destination
    // is otherwise the same RMW surface as ADDQ: absolute destinations retain
    // both read/write facts, while the add-family commit owner handles (An)+
    // and -(An) without exposing a partial update on route failure.
    if (operation.destination_ea.mode != M68kEaMode::data_register &&
        m68k_c4_auto_update_class(operation.destination_ea.mode) == M68kC4AutoUpdateClass::none) {
      check_fact(operation.destination_ea, M68kC4OperandRole::destination,
                 M68kStaticMemoryFactRole::destination_read);
      check_fact(operation.destination_ea, M68kC4OperandRole::destination,
                 M68kStaticMemoryFactRole::destination_write);
    }
  } else if (operation.kind == M68kIrKind::add) {
    // SEG-007-T145: an ADD operand using an auto-updating addressing mode is
    // now lowered by the add-family deferred-address-commit path in
    // emit_m68k_operation_c (see
    // docs/architecture/c4-add-family-auto-update-commit-contract.md); it is
    // no longer a requires_architecture_decision gap. A non-auto-update
    // foldable EA still needs its retained fact.
    const auto check_add_ea = [&](const M68kEffectiveAddress &ea, M68kC4OperandRole role,
                                  M68kStaticMemoryFactRole fact_role) {
      if (m68k_c4_auto_update_class(ea.mode) == M68kC4AutoUpdateClass::none)
        check_fact(ea, role, fact_role);
    };
    check_add_ea(operation.source_ea, M68kC4OperandRole::source, M68kStaticMemoryFactRole::source_read);
    if (operation.destination_ea.mode != M68kEaMode::data_register &&
        operation.destination_ea.mode != M68kEaMode::address_register) {
      check_add_ea(operation.destination_ea, M68kC4OperandRole::destination, M68kStaticMemoryFactRole::destination_read);
      check_add_ea(operation.destination_ea, M68kC4OperandRole::destination, M68kStaticMemoryFactRole::destination_write);
    }
  } else if (operation.kind == M68kIrKind::add_address) {
    // SEG-007-T145 / SEG-021-T006: ADDA with an auto-updating source, including the same-register
    // aliasing shape `ADDA <auto>(An),An` (Musashi-pinned order, lowered by the add-family deferred-
    // address-commit path), is fully lowered and produces no gap row.
    if (m68k_c4_auto_update_class(operation.source_ea.mode) == M68kC4AutoUpdateClass::none)
      check_fact(operation.source_ea, M68kC4OperandRole::source, M68kStaticMemoryFactRole::source_read);
  } else if (operation.kind == M68kIrKind::subtract_address || operation.kind == M68kIrKind::compare_address ||
             operation.kind == M68kIrKind::compare) {
    // SEG-021-T006: SUBA/CMPA/CMP with an auto-updating source are lowered by the arithmetic-family
    // deferred-address-commit helper in emit_m68k_operation_c (routed: one An snapshot, commit after
    // every routed access, PC last); they are no longer requires_architecture_decision gaps. A non-
    // auto-update foldable source still needs its retained fact.
    if (m68k_c4_auto_update_class(operation.source_ea.mode) == M68kC4AutoUpdateClass::none)
      check_fact(operation.source_ea, M68kC4OperandRole::source, M68kStaticMemoryFactRole::source_read);
  } else if (operation.kind == M68kIrKind::multiply_signed_word ||
             operation.kind == M68kIrKind::multiply_unsigned_word ||
             operation.kind == M68kIrKind::divide_signed_word ||
             operation.kind == M68kIrKind::divide_unsigned_word) {
    // SEG-007-T220: MULS.W <ea>,Dn (and MULU/DIVS/DIVU) share a read-only-source,
    // fixed-Dn-destination shape. Their destination is architecturally always Dn,
    // never needing a fact. SEG-021-T010: an auto-updating source is lowered by the
    // operation-local deferred-address-commit helper in emit_m68k_operation_c
    // (`m68k_emit_routed_muldiv_auto_update`, one An snapshot, commit strictly after
    // the routed read/postincrement, PC last), the same technique CMP/SUBA/CMPA
    // gained in SEG-021-T006 and AND/OR/EOR/BTST/BCHG/BCLR/BSET gained in
    // SEG-021-T007/T008; it is no longer a requires_architecture_decision gap.
    if (m68k_c4_auto_update_class(operation.source_ea.mode) == M68kC4AutoUpdateClass::none)
      check_fact(operation.source_ea, M68kC4OperandRole::source, M68kStaticMemoryFactRole::source_read);
  } else if (operation.kind == M68kIrKind::write_status_register ||
             operation.kind == M68kIrKind::write_condition_codes ||
             operation.kind == M68kIrKind::check_bounds) {  // SEG-021-T019: CHK.W reads its word bound
    // SEG-021-T018: MOVE <ea>,SR / MOVE <ea>,CCR read one word source; an auto-updating source is lowered by
    // the operation-local deferred commit, and a non-auto foldable source needs its retained fact.
    if (m68k_c4_auto_update_class(operation.source_ea.mode) == M68kC4AutoUpdateClass::none)
      check_fact(operation.source_ea, M68kC4OperandRole::source, M68kStaticMemoryFactRole::source_read);
  } else if (operation.kind == M68kIrKind::read_status_register) {
    // SEG-021-T018: a memory MOVE from SR destination is read before it is written (memory-Scc shape).
    if (operation.destination_ea.mode != M68kEaMode::data_register &&
        m68k_c4_auto_update_class(operation.destination_ea.mode) == M68kC4AutoUpdateClass::none) {
      check_fact(operation.destination_ea, M68kC4OperandRole::destination, M68kStaticMemoryFactRole::destination_read);
      check_fact(operation.destination_ea, M68kC4OperandRole::destination, M68kStaticMemoryFactRole::destination_write);
    }
  } else if (operation.kind == M68kIrKind::subtract) {
    // SEG-007-T170 / SEG-021-T006: SUB (`<ea>,Dn` or `Dn,<ea>`) shares ADD's two-operand shape; an
    // auto-updating operand is lowered by the arithmetic-family deferred-address-commit helper (routed
    // C4 context), so it produces no gap row. A non-auto foldable EA still needs its retained facts.
    if (m68k_c4_auto_update_class(operation.source_ea.mode) == M68kC4AutoUpdateClass::none)
      check_fact(operation.source_ea, M68kC4OperandRole::source, M68kStaticMemoryFactRole::source_read);
    if (operation.destination_ea.mode != M68kEaMode::data_register &&
        operation.destination_ea.mode != M68kEaMode::address_register &&
        m68k_c4_auto_update_class(operation.destination_ea.mode) == M68kC4AutoUpdateClass::none) {
      check_fact(operation.destination_ea, M68kC4OperandRole::destination, M68kStaticMemoryFactRole::destination_read);
      check_fact(operation.destination_ea, M68kC4OperandRole::destination, M68kStaticMemoryFactRole::destination_write);
    }
  } else if (operation.kind == M68kIrKind::subtract_immediate) {
    // SEG-007-T202: an auto-updating ((An)+/-(An)) SUBI destination is now
    // lowered by the subtract-family deferred-address-commit path in
    // emit_m68k_operation_c, mirroring the add-family's ADDQ/ADDI extension
    // (docs/architecture/c4-add-family-auto-update-commit-contract.md); it is
    // no longer a requires_architecture_decision gap. SUBI's source is
    // always the instruction-embedded immediate (never a memory read or an
    // auto-updating mode), so no source check is needed. A non-auto-update
    // foldable memory destination still needs its retained read+write fact,
    // exactly mirroring ADDI's own destination-fact treatment above.
    if (operation.destination_ea.mode != M68kEaMode::data_register &&
        operation.destination_ea.mode != M68kEaMode::address_register &&
        m68k_c4_auto_update_class(operation.destination_ea.mode) == M68kC4AutoUpdateClass::none) {
      check_fact(operation.destination_ea, M68kC4OperandRole::destination, M68kStaticMemoryFactRole::destination_read);
      check_fact(operation.destination_ea, M68kC4OperandRole::destination, M68kStaticMemoryFactRole::destination_write);
    }
  } else if (operation.kind == M68kIrKind::compare_immediate) {
    // SEG-007-T146 / SEG-021-T006: `CMPI #imm,<ea>` has an immediate source; its destination is read
    // (never written) to form the CCR-only result. An auto-updating destination is lowered by the
    // arithmetic-family deferred-address-commit helper and produces no gap row.
    if (m68k_c4_auto_update_class(operation.destination_ea.mode) == M68kC4AutoUpdateClass::none)
      check_fact(operation.destination_ea, M68kC4OperandRole::destination, M68kStaticMemoryFactRole::destination_read);
  } else if (operation.kind == M68kIrKind::bit_test) {
    // SEG-021-T008: an auto-updating destination lowers through the bit-family deferred address commit.
    if (m68k_c4_auto_update_class(operation.destination_ea.mode) == M68kC4AutoUpdateClass::none)
      check_fact(operation.destination_ea, M68kC4OperandRole::destination, M68kStaticMemoryFactRole::destination_read);
  } else if (operation.kind == M68kIrKind::bit_change || operation.kind == M68kIrKind::bit_clear ||
             operation.kind == M68kIrKind::bit_set) {
    // SEG-007-T209: BCHG/BCLR/BSET read-modify-write their destination (unlike
    // BTST's read-only destination above), so this branch checks the
    // destination_write fact -- the same "read side threaded from the write
    // fact" RMW shape already established for SUBQ/SUBI/write_clr, not
    // BTST's own destination_read shape.
    // SEG-021-T008: an auto-updating destination lowers through the bit-family deferred address commit.
    if (m68k_c4_auto_update_class(operation.destination_ea.mode) == M68kC4AutoUpdateClass::none)
      check_fact(operation.destination_ea, M68kC4OperandRole::destination, M68kStaticMemoryFactRole::destination_write);
  } else if (operation.kind == M68kIrKind::write_clr) {
    // SEG-007-T157 / ADR-0019 Stage B: an auto-updating `(An)+` / `-(An)`
    // CLR destination is now lowered by the established deferred-address-
    // commit path in emit_m68k_operation_c (the same pattern the add
    // family, MOVE, and MOVEA already use); it is no longer a
    // requires_architecture_decision gap. A non-auto-update foldable
    // destination still needs its retained fact exactly as before.
    if (m68k_c4_auto_update_class(operation.destination_ea.mode) == M68kC4AutoUpdateClass::none)
      check_fact(operation.destination_ea, M68kC4OperandRole::destination, M68kStaticMemoryFactRole::destination_write);
  } else if (operation.kind == M68kIrKind::logical_not || operation.kind == M68kIrKind::shift_rotate_memory ||
             operation.kind == M68kIrKind::negate_word || operation.kind == M68kIrKind::negate_extended ||
             operation.kind == M68kIrKind::negate_decimal || operation.kind == M68kIrKind::test_and_set ||
             operation.kind == M68kIrKind::set_conditional) {
    // SEG-021-T016: TAS and memory Scc (read then write) share it too.
    // SEG-021-T014: NEG/NEGX share NOT's one-address RMW gap shape.
    // SEG-007-T168: NOT has no source operand at all (unlike the sibling
    // logical family AND/OR/EOR/ANDI/ORI/EORI, which always carry one, just
    // never a memory one for the immediate forms). Its sole destination is
    // a full RMW operand, mirroring SUBQ's own destination shape: a Dn/An
    // destination needs nothing; an auto-updating destination is declined
    // cleanly via requires_architecture_decision (the logical family carries
    // no deferred-address-commit contract, matching its AND/OR/EOR/ANDI/
    // ORI/EORI siblings); a non-auto foldable memory destination needs both
    // destination_read and destination_write facts.
    if (operation.destination_ea.mode != M68kEaMode::data_register &&
        operation.destination_ea.mode != M68kEaMode::address_register) {
      // SEG-021-T005: an auto-updating destination is lowered by the NOT
      // deferred-address-commit path in emit_m68k_operation_c (no gap row).
      if (m68k_c4_auto_update_class(operation.destination_ea.mode) == M68kC4AutoUpdateClass::none) {
        check_fact(operation.destination_ea, M68kC4OperandRole::destination,
                   M68kStaticMemoryFactRole::destination_read);
        check_fact(operation.destination_ea, M68kC4OperandRole::destination,
                   M68kStaticMemoryFactRole::destination_write);
      }
    }
  } else if (operation.kind == M68kIrKind::test_operand ||
             operation.kind == M68kIrKind::logical_and_immediate ||
             operation.kind == M68kIrKind::logical_or_immediate ||
             operation.kind == M68kIrKind::exclusive_or_immediate) {
    // SEG-007-T167: ORI / EORI (`#imm,<ea>`) share ANDI's exact C4 gap shape
    // -- an instruction-embedded immediate source (never a memory read), a
    // read-modify-write destination whose retained destination_write fact is
    // the one checked shape, and an auto-updating destination declined
    // cleanly to the established frontier (the logical family has no
    // deferred-address-commit contract).
    const auto &ea = operation.kind == M68kIrKind::test_operand ? operation.source_ea : operation.destination_ea;
    const auto role = operation.kind == M68kIrKind::test_operand ? M68kC4OperandRole::source : M68kC4OperandRole::destination;
    const auto fact_role = operation.kind == M68kIrKind::test_operand ? M68kStaticMemoryFactRole::source_read
                                                                       : M68kStaticMemoryFactRole::destination_write;
    if (const auto update = m68k_c4_auto_update_class(ea.mode); update != M68kC4AutoUpdateClass::none) {
      // SEG-021-T005: TST lowers its own auto-updating operand through the deferred address-register
      // commit (test_operand case of emit_m68k_operation_c); only the ANDI/ORI/EORI destinations still decline.
      // SEG-021-T007: ANDI/ORI/EORI destinations likewise lower through the logical-family deferred
      // address-register commit helper; no gap row for any of the three.
      (void)update;
    } else {
      check_fact(ea, role, fact_role);
    }
  } else if (operation.kind == M68kIrKind::logical_and ||
             operation.kind == M68kIrKind::logical_or ||
             operation.kind == M68kIrKind::exclusive_or) {
    // SEG-007-T167: AND/OR/EOR (`<ea>,Dn` or `Dn,<ea>`). Exactly one operand
    // is a data register; the other may be a foldable-control memory EA. An
    // auto-updating operand is declined cleanly via
    // requires_architecture_decision (no deferred-address-commit contract for
    // this family, matching CMP/SUBA/CMPA and the sibling ANDI/ORI/EORI). A
    // non-auto foldable memory source needs source_read; a non-auto foldable
    // memory RMW destination needs destination_read + destination_write,
    // mirroring `add`'s own destination shape.
    // SEG-021-T007: an auto-updating operand is lowered by the logical-family deferred-address-commit
    // helper (emit_m68k_operation_c, routed): no gap row. A non-auto foldable operand still needs its facts.
    const bool declined = m68k_c4_auto_update_class(operation.source_ea.mode) != M68kC4AutoUpdateClass::none ||
                          m68k_c4_auto_update_class(operation.destination_ea.mode) != M68kC4AutoUpdateClass::none;
    if (!declined) {
      check_fact(operation.source_ea, M68kC4OperandRole::source, M68kStaticMemoryFactRole::source_read);
      if (operation.destination_ea.mode != M68kEaMode::data_register &&
          operation.destination_ea.mode != M68kEaMode::address_register) {
        check_fact(operation.destination_ea, M68kC4OperandRole::destination,
                   M68kStaticMemoryFactRole::destination_read);
        check_fact(operation.destination_ea, M68kC4OperandRole::destination,
                   M68kStaticMemoryFactRole::destination_write);
      }
    }
  }
  return rows;
}

// SEG-007-T142 correction (ADR-0015 Decision §7): one finite, exhaustive,
// injective `GenesisC4LoweringDimensions` literal per distinct (ir_kind, gap)
// shape `classify_m68k_c4_gap_shapes` can currently produce. Deliberately no
// default/catch-all case: an (ir_kind, gap) pair this switch does not name is
// not yet a representable C4 lowering-gap shape, and the caller must reject
// translation rather than report ambiguous "other" evidence.
std::optional<std::string_view> c4_lowering_dimension_literal(M68kIrKind kind, M68kC4GapClass gap) {
  switch (gap) {
  case M68kC4GapClass::missing_dispatcher:
    switch (kind) {
    case M68kIrKind::branch_ne_short: return "GENESIS_C4_LOWERING_DIMENSIONS_BRANCH_NE_SHORT_MISSING_DISPATCHER";
    case M68kIrKind::branch_always_short: return "GENESIS_C4_LOWERING_DIMENSIONS_BRANCH_ALWAYS_SHORT_MISSING_DISPATCHER";
    case M68kIrKind::compare: return "GENESIS_C4_LOWERING_DIMENSIONS_COMPARE_MISSING_DISPATCHER";
    case M68kIrKind::compare_immediate: return "GENESIS_C4_LOWERING_DIMENSIONS_COMPARE_IMMEDIATE_MISSING_DISPATCHER";
    case M68kIrKind::add_immediate: return "GENESIS_C4_LOWERING_DIMENSIONS_ADD_IMMEDIATE_MISSING_DISPATCHER";
    case M68kIrKind::add_quick: return "GENESIS_C4_LOWERING_DIMENSIONS_ADD_QUICK_MISSING_DISPATCHER";
    case M68kIrKind::subtract: return "GENESIS_C4_LOWERING_DIMENSIONS_SUBTRACT_MISSING_DISPATCHER";
    case M68kIrKind::subtract_immediate: return "GENESIS_C4_LOWERING_DIMENSIONS_SUBTRACT_IMMEDIATE_MISSING_DISPATCHER";
    case M68kIrKind::logical_and: return "GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_AND_MISSING_DISPATCHER";
    case M68kIrKind::logical_or: return "GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_OR_MISSING_DISPATCHER";
    case M68kIrKind::logical_or_immediate: return "GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_OR_IMMEDIATE_MISSING_DISPATCHER";
    case M68kIrKind::exclusive_or: return "GENESIS_C4_LOWERING_DIMENSIONS_EXCLUSIVE_OR_MISSING_DISPATCHER";
    case M68kIrKind::exclusive_or_immediate: return "GENESIS_C4_LOWERING_DIMENSIONS_EXCLUSIVE_OR_IMMEDIATE_MISSING_DISPATCHER";
    case M68kIrKind::write_swap: return "GENESIS_C4_LOWERING_DIMENSIONS_WRITE_SWAP_MISSING_DISPATCHER";
    case M68kIrKind::sign_extend_word: return "GENESIS_C4_LOWERING_DIMENSIONS_SIGN_EXTEND_WORD_MISSING_DISPATCHER";
    case M68kIrKind::sign_extend_long: return "GENESIS_C4_LOWERING_DIMENSIONS_SIGN_EXTEND_LONG_MISSING_DISPATCHER";
    case M68kIrKind::push_effective_address: return "GENESIS_C4_LOWERING_DIMENSIONS_PUSH_EFFECTIVE_ADDRESS_MISSING_DISPATCHER";
    case M68kIrKind::link_frame: return "GENESIS_C4_LOWERING_DIMENSIONS_LINK_FRAME_MISSING_DISPATCHER";
    case M68kIrKind::unlink_frame: return "GENESIS_C4_LOWERING_DIMENSIONS_UNLINK_FRAME_MISSING_DISPATCHER";
    case M68kIrKind::bit_change: return "GENESIS_C4_LOWERING_DIMENSIONS_BIT_CHANGE_MISSING_DISPATCHER";
    case M68kIrKind::bit_clear: return "GENESIS_C4_LOWERING_DIMENSIONS_BIT_CLEAR_MISSING_DISPATCHER";
    case M68kIrKind::bit_set: return "GENESIS_C4_LOWERING_DIMENSIONS_BIT_SET_MISSING_DISPATCHER";
    case M68kIrKind::shift_rotate_register: return "GENESIS_C4_LOWERING_DIMENSIONS_SHIFT_ROTATE_REGISTER_MISSING_DISPATCHER";
    case M68kIrKind::shift_rotate_memory: return "GENESIS_C4_LOWERING_DIMENSIONS_SHIFT_ROTATE_MEMORY_MISSING_DISPATCHER";
    default: return std::nullopt;
    }
  case M68kC4GapClass::missing_routing:
    // A statically foldable-but-unroutable MOVEM source is advisory-only in
    // the preflight inventory; the emitter still lowers it through the
    // ordinary runtime genesis_route_access call and never turns it into an
    // emitted C4 lowering-gap stop (see the caller below), so this gap
    // class deliberately has no dimension literal.
    (void)kind;
    return std::nullopt;
  case M68kC4GapClass::requires_architecture_decision:
    switch (kind) {
    // SEG-007-T145: `add` auto-update operands are now lowered by the
    // deferred-address-commit path, so `classify_m68k_c4_gap_shapes` no longer
    // produces this (add, requires_architecture_decision) pair; the literal is
    // retained for the stable GenesisC4LoweringDimensions enumeration only.
    // SEG-021-T006: the ADDA same-register aliasing shape is lowered too, so the
    // ADD/ADDA/SUB/SUBA/CMP/CMPA/CMPI auto-update literals below are likewise
    // retained for the stable enumeration only.
    case M68kIrKind::add: return "GENESIS_C4_LOWERING_DIMENSIONS_ADD_AUTO_UPDATE";
    case M68kIrKind::write_clr: return "GENESIS_C4_LOWERING_DIMENSIONS_CLR_AUTO_UPDATE";
    case M68kIrKind::write_movea: return "GENESIS_C4_LOWERING_DIMENSIONS_MOVEA_AUTO_UPDATE";
    case M68kIrKind::add_address: return "GENESIS_C4_LOWERING_DIMENSIONS_ADDA_AUTO_UPDATE";
    case M68kIrKind::subtract_address: return "GENESIS_C4_LOWERING_DIMENSIONS_SUBA_AUTO_UPDATE";
    case M68kIrKind::compare_address: return "GENESIS_C4_LOWERING_DIMENSIONS_CMPA_AUTO_UPDATE";
    case M68kIrKind::compare: return "GENESIS_C4_LOWERING_DIMENSIONS_CMP_AUTO_UPDATE";
    case M68kIrKind::compare_immediate: return "GENESIS_C4_LOWERING_DIMENSIONS_CMPI_AUTO_UPDATE";
    case M68kIrKind::bit_test: return "GENESIS_C4_LOWERING_DIMENSIONS_BIT_TEST_AUTO_UPDATE";
    // SEG-007-T209: BCHG/BCLR/BSET share BTST's declined-auto-update shape --
    // this family also carries no deferred-address-commit contract.
    case M68kIrKind::bit_change: return "GENESIS_C4_LOWERING_DIMENSIONS_BIT_CHANGE_AUTO_UPDATE";
    case M68kIrKind::bit_clear: return "GENESIS_C4_LOWERING_DIMENSIONS_BIT_CLEAR_AUTO_UPDATE";
    case M68kIrKind::bit_set: return "GENESIS_C4_LOWERING_DIMENSIONS_BIT_SET_AUTO_UPDATE";
    case M68kIrKind::test_operand: return "GENESIS_C4_LOWERING_DIMENSIONS_TST_AUTO_UPDATE";
    case M68kIrKind::logical_and_immediate: return "GENESIS_C4_LOWERING_DIMENSIONS_ANDI_AUTO_UPDATE";
    // SEG-007-T167: logical family auto-update declines (no deferred commit).
    case M68kIrKind::logical_and: return "GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_AND_AUTO_UPDATE";
    case M68kIrKind::logical_or: return "GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_OR_AUTO_UPDATE";
    case M68kIrKind::logical_or_immediate: return "GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_OR_IMMEDIATE_AUTO_UPDATE";
    case M68kIrKind::exclusive_or: return "GENESIS_C4_LOWERING_DIMENSIONS_EXCLUSIVE_OR_AUTO_UPDATE";
    case M68kIrKind::exclusive_or_immediate: return "GENESIS_C4_LOWERING_DIMENSIONS_EXCLUSIVE_OR_IMMEDIATE_AUTO_UPDATE";
    // SEG-007-T168: NOT shares the sibling logical family's declined-
    // auto-update shape (no deferred commit contract).
    case M68kIrKind::logical_not: return "GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_NOT_AUTO_UPDATE";
    // SEG-007-T170: SUB/SUBI decline an auto-updating operand cleanly,
    // mirroring their own already-represented sibling SUBA above.
    case M68kIrKind::subtract: return "GENESIS_C4_LOWERING_DIMENSIONS_SUBTRACT_AUTO_UPDATE";
    case M68kIrKind::subtract_immediate: return "GENESIS_C4_LOWERING_DIMENSIONS_SUBTRACT_IMMEDIATE_AUTO_UPDATE";
    // SEG-021-T010: MULS/MULU/DIVS/DIVU auto-update operands are now lowered by
    // `m68k_emit_routed_muldiv_auto_update`'s deferred-address-commit path, so
    // `classify_m68k_c4_gap_shapes` no longer produces this (kind,
    // requires_architecture_decision) pair for any of the four mnemonics; these four
    // literals are retained for the stable GenesisC4LoweringDimensions enumeration only,
    // the same precedent the ADD-family literals above already establish.
    case M68kIrKind::multiply_signed_word: return "GENESIS_C4_LOWERING_DIMENSIONS_MULS_AUTO_UPDATE";
    case M68kIrKind::multiply_unsigned_word: return "GENESIS_C4_LOWERING_DIMENSIONS_MULU_AUTO_UPDATE";
    case M68kIrKind::divide_signed_word: return "GENESIS_C4_LOWERING_DIMENSIONS_DIVS_AUTO_UPDATE";
    case M68kIrKind::divide_unsigned_word: return "GENESIS_C4_LOWERING_DIMENSIONS_DIVU_AUTO_UPDATE";
    default: return std::nullopt;
    }
  case M68kC4GapClass::missing_fact:
    switch (kind) {
    case M68kIrKind::write_move: return "GENESIS_C4_LOWERING_DIMENSIONS_MOVE_MISSING_FACT";
    case M68kIrKind::write_movea: return "GENESIS_C4_LOWERING_DIMENSIONS_MOVEA_MISSING_FACT";
    case M68kIrKind::add: return "GENESIS_C4_LOWERING_DIMENSIONS_ADD_MISSING_FACT";
    case M68kIrKind::add_quick: return "GENESIS_C4_LOWERING_DIMENSIONS_ADD_QUICK_MISSING_FACT";
    case M68kIrKind::add_immediate: return "GENESIS_C4_LOWERING_DIMENSIONS_ADD_IMMEDIATE_MISSING_FACT";
    case M68kIrKind::add_address: return "GENESIS_C4_LOWERING_DIMENSIONS_ADDA_MISSING_FACT";
    case M68kIrKind::subtract_address: return "GENESIS_C4_LOWERING_DIMENSIONS_SUBA_MISSING_FACT";
    case M68kIrKind::compare_address: return "GENESIS_C4_LOWERING_DIMENSIONS_CMPA_MISSING_FACT";
    case M68kIrKind::compare: return "GENESIS_C4_LOWERING_DIMENSIONS_CMP_MISSING_FACT";
    case M68kIrKind::compare_immediate: return "GENESIS_C4_LOWERING_DIMENSIONS_CMPI_MISSING_FACT";
    // SEG-007-T209: represented BCHG/BCLR/BSET with a statically foldable
    // memory destination and no retained resolver fact (mirrors the
    // write-side-only SUBQ/SUBI/CLR pattern, not BTST's read-side one).
    case M68kIrKind::bit_change: return "GENESIS_C4_LOWERING_DIMENSIONS_BIT_CHANGE_MISSING_FACT";
    case M68kIrKind::bit_clear: return "GENESIS_C4_LOWERING_DIMENSIONS_BIT_CLEAR_MISSING_FACT";
    case M68kIrKind::bit_set: return "GENESIS_C4_LOWERING_DIMENSIONS_BIT_SET_MISSING_FACT";
    case M68kIrKind::bit_test: return "GENESIS_C4_LOWERING_DIMENSIONS_BIT_TEST_MISSING_FACT";
    case M68kIrKind::test_operand: return "GENESIS_C4_LOWERING_DIMENSIONS_TST_MISSING_FACT";
    case M68kIrKind::write_clr: return "GENESIS_C4_LOWERING_DIMENSIONS_CLR_MISSING_FACT";
    case M68kIrKind::logical_and_immediate: return "GENESIS_C4_LOWERING_DIMENSIONS_ANDI_MISSING_FACT";
    // SEG-007-T167: represented logical-family foldable memory operand, no fact.
    case M68kIrKind::logical_and: return "GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_AND_MISSING_FACT";
    case M68kIrKind::logical_or: return "GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_OR_MISSING_FACT";
    case M68kIrKind::logical_or_immediate: return "GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_OR_IMMEDIATE_MISSING_FACT";
    case M68kIrKind::exclusive_or: return "GENESIS_C4_LOWERING_DIMENSIONS_EXCLUSIVE_OR_MISSING_FACT";
    case M68kIrKind::exclusive_or_immediate: return "GENESIS_C4_LOWERING_DIMENSIONS_EXCLUSIVE_OR_IMMEDIATE_MISSING_FACT";
    // SEG-007-T168: represented NOT with a statically foldable memory
    // destination and no retained resolver fact.
    case M68kIrKind::logical_not: return "GENESIS_C4_LOWERING_DIMENSIONS_LOGICAL_NOT_MISSING_FACT";
    // SEG-021-T009: represented memory-word shift/rotate with a foldable destination and no retained fact.
    case M68kIrKind::shift_rotate_memory: return "GENESIS_C4_LOWERING_DIMENSIONS_SHIFT_ROTATE_MEMORY_MISSING_FACT";
    // SEG-007-T170: represented SUB/SUBI with a statically foldable memory
    // operand and no retained resolver fact (mirrors ADD_MISSING_FACT).
    case M68kIrKind::subtract: return "GENESIS_C4_LOWERING_DIMENSIONS_SUBTRACT_MISSING_FACT";
    case M68kIrKind::subtract_immediate: return "GENESIS_C4_LOWERING_DIMENSIONS_SUBTRACT_IMMEDIATE_MISSING_FACT";
    // SEG-007-T220: represented MULS.W with a statically foldable memory
    // source and no retained resolver fact (mirrors CMP_MISSING_FACT).
    case M68kIrKind::multiply_signed_word: return "GENESIS_C4_LOWERING_DIMENSIONS_MULS_MISSING_FACT";
    case M68kIrKind::multiply_unsigned_word: return "GENESIS_C4_LOWERING_DIMENSIONS_MULU_MISSING_FACT";
    case M68kIrKind::divide_signed_word: return "GENESIS_C4_LOWERING_DIMENSIONS_DIVS_MISSING_FACT";
    case M68kIrKind::divide_unsigned_word: return "GENESIS_C4_LOWERING_DIMENSIONS_DIVU_MISSING_FACT";
    default: return std::nullopt;
    }
  }
  return std::nullopt;
}

M68kC4Preflight preflight_m68k_general_startup_c4(const FrontendPartialProgram &partial) {
  M68kC4Preflight result{.valid = false, .rows = {}};
  const auto &prefix = partial.accepted_prefix;
  if (prefix.decoded.size() != prefix.ir.size() || prefix.static_blocks.empty()) return result;

  std::map<Address, const M68kDecodedInstruction *> decoded;
  std::map<Address, const M68kIrOperation *> operations;
  for (std::size_t index = 0; index < prefix.decoded.size(); ++index) {
    const auto &instruction = prefix.decoded[index];
    const auto &operation = prefix.ir[index];
    if (!independently_decoded_and_lifted(instruction, operation) ||
        !decoded.emplace(instruction.provenance.source.address.value, &instruction).second ||
        !operations.emplace(operation.provenance.source.address.value, &operation).second)
      return result;
  }

  std::set<std::pair<Address, M68kStaticMemoryFactRole>> facts;
  for (const auto &fact : prefix.static_memory_facts) {
    if (!valid_c4_static_memory_fact(fact, decoded) ||
        !facts.emplace(fact.operation.source.address.value, fact.role).second)
      return result;
  }
  // SEG-007-T075: any invalid/forged M68kMovemAdjacentLeaFact rejects the
  // whole preflight, exactly like a forged M68kStaticMemoryFact above -- a
  // fact that fails independent re-verification is evidence of an
  // inconsistent retained prefix, never a silently skipped optimization.
  {
    std::set<Address> movem_lea_consumers;
    for (const auto &fact : prefix.movem_adjacent_lea_facts) {
      if (!valid_c4_movem_adjacent_lea_fact(fact, decoded, prefix.static_blocks, prefix.static_edges,
                                            prefix.mapping_claims) ||
          !movem_lea_consumers.emplace(fact.consumer.source.address.value).second)
        return result;
    }
  }
  // SEG-007-T077: any invalid/forged M68kOwnedCartridgeRegionFact rejects the
  // whole preflight, exactly like a forged M68kMovemAdjacentLeaFact above --
  // never a silently skipped region. A duplicate fact naming the identical
  // claim is rejected the same way a duplicate movem-adjacent-LEA consumer
  // is above, matching the established forged-fact-duplicate precedent.
  {
    std::set<std::tuple<std::string, std::uint32_t, std::uint32_t, std::uint64_t, std::uint64_t>>
        owned_region_claims_seen;
    for (const auto &fact : prefix.owned_cartridge_region_facts) {
      if (!valid_c4_owned_cartridge_region_fact(fact, prefix.mapping_claims) ||
          !owned_region_claims_seen
               .emplace(fact.claim.name, fact.claim.target_begin.value, fact.claim.target_end.value,
                        fact.claim.image_begin.value, fact.claim.image_end.value)
               .second)
        return result;
    }
  }

  // SEG-007-T142 correction: family() is display-only bookkeeping the shared
  // classifier does not need; every actual gap decision below now comes from
  // classify_m68k_c4_gap_shapes, the same per-operation classifier each
  // generated genesis_c4_lowering_stop_<addr> consults for its own dimension
  // literal, so the advisory inventory below and the runtime stop can never
  // silently diverge.
  const auto family = [](M68kIrKind kind) -> std::string_view {
    switch (kind) {
    case M68kIrKind::write_move: return "move";
    case M68kIrKind::write_movea: return "movea";
    case M68kIrKind::add: return "add";
    case M68kIrKind::add_address: return "adda";
    case M68kIrKind::subtract_address: return "suba";
    case M68kIrKind::compare_address: return "cmpa";
    case M68kIrKind::compare: return "cmp";
    case M68kIrKind::compare_immediate: return "cmpi";
    case M68kIrKind::bit_test: return "bit_test";
    case M68kIrKind::dbcc_loop: return "dbcc";
    case M68kIrKind::movem_transfer: return "movem";
    case M68kIrKind::logical_and_immediate: return "andi";
    case M68kIrKind::logical_and: return "and";
    case M68kIrKind::logical_or: return "or";
    case M68kIrKind::logical_or_immediate: return "ori";
    case M68kIrKind::exclusive_or: return "eor";
    case M68kIrKind::exclusive_or_immediate: return "eori";
    case M68kIrKind::test_operand: return "tst";
    case M68kIrKind::write_clr: return "clr";
    case M68kIrKind::logical_not: return "not";
    case M68kIrKind::shift_rotate_memory: return "shift_rotate_memory";
    case M68kIrKind::multiply_signed_word: return "muls";
    case M68kIrKind::multiply_unsigned_word: return "mulu";
    case M68kIrKind::divide_signed_word: return "divs";
    case M68kIrKind::divide_unsigned_word: return "divu";
    default: return "other";
    }
  };
  for (const auto &[address, operation] : operations) {
    (void)address;
    for (const auto &shape : classify_m68k_c4_gap_shapes(*operation, facts, prefix.mapping_claims))
      result.rows.push_back({family(operation->kind), operation->kind, shape.operand_role, shape.width,
                             shape.ea_class, shape.auto_update, shape.gap, shape.predecessor});
  }
  std::sort(result.rows.begin(), result.rows.end(), [](const auto &left, const auto &right) {
    return std::tie(left.family, left.ir_kind, left.operand_role, left.width, left.ea_class, left.auto_update,
                    left.gap, left.predecessor) <
           std::tie(right.family, right.ir_kind, right.operand_role, right.width, right.ea_class,
                    right.auto_update, right.gap, right.predecessor);
  });
  result.rows.erase(std::unique(result.rows.begin(), result.rows.end(), [](const auto &left, const auto &right) {
                      return std::tie(left.family, left.ir_kind, left.operand_role, left.width, left.ea_class,
                                      left.auto_update, left.gap, left.predecessor) ==
                             std::tie(right.family, right.ir_kind, right.operand_role, right.width, right.ea_class,
                                      right.auto_update, right.gap, right.predecessor);
                    }), result.rows.end());
  result.valid = true;
  return result;
}

std::string emit_m68k_general_startup_runtime_c_to(std::ostream &out, std::string_view header,
                                                       const FrontendPartialProgram &partial) {
  const auto preflight = preflight_m68k_general_startup_c4(partial);
  if (!preflight.valid) return "/* translation rejected: invalid C4 retained prefix */\n";
  const auto aot_entries = validated_immutable_rom_aot_entries(partial.accepted_prefix);
  if (!aot_entries) return "/* translation rejected: invalid immutable-ROM AOT entry */\n";
  // SEG-007-T252 / ADR-0040: the former SEG-007-T157 / ADR-0019 READ-progress
  // region resolution (`read_region_bounds`) fed only the now-removed guarded
  // watchdog READ note; termination/progress policy belongs to the runner.
  // Preflight is deliberately advisory here.  Each unlowerable retained
  // instruction is cut locally below; its old address-free row remains useful
  // to callers that report static capability inventory.
  // SEG-007-T064 (superseded by SEG-007-T183 / ADR-0028 §8, corrected by
  // SEG-007-T205): only emptiness is rejected here. Before ADR-0028 §8,
  // `partial.frontiers` was itself bounded to
  // `1..m68k_discovery_max_frontier_exits` by discovery's own construction,
  // so the upper-bound half of this check was a real (if redundant)
  // representability guard. ADR-0028 §8 / SEG-007-T183 changed discovery to
  // reassign `frontiers` to the COMPLETE, untruncated semantic-frontier
  // obligation set (`recompiler_sort_dedup_and_project_frontiers(...).complete`)
  // -- `m68k_discovery_max_frontier_exits` became a bound on the separate
  // deterministic diagnostic projection/residual-count pair only, and is no
  // longer applied to `partial.frontiers` at construction time. Every
  // downstream per-frontier consumer below (sibling_frontier_addresses,
  // the frontier_stop_names/frontier_addresses build, and the per-block
  // indirect/terminal checks) already iterates the complete set with no
  // assumption that its size is capped, so re-imposing the stale cap here
  // rejected an otherwise fully representable above-cap complete obligation
  // set before it ever reached those checks. Only genuine unrepresentability
  // (an empty set, meaning discovery produced no frontier obligation at all)
  // is rejected; cardinality alone is never disqualifying.
  if (partial.frontiers.empty())
    return "/* translation rejected: unrepresentable C4 frontier */\n";
  // Every exit's diagnostic.provenance must be present and carry a distinct
  // source address before any per-exit stop function can be named; this is
  // re-checked per exit by build_genesis_frontier_stop_function below, but
  // the address is also needed here to key the dispatcher-tail lookup.
  // SEG-007-T064: the full set of every retained exit's own source address,
  // threaded into every runtime_frontier_eligible re-check below so a block
  // whose two edges reach two different retained exits does not get
  // rejected purely because one exit's own single-address view does not
  // recognize the other's.
  std::set<Address> sibling_frontier_addresses;
  for (const auto &frontier : partial.frontiers)
    if (frontier.diagnostic.provenance) sibling_frontier_addresses.insert(frontier.diagnostic.provenance->source.address.value);
  // SEG-007-T142 correction: reuse preflight_m68k_general_startup_c4's own
  // (address, role) fact-membership set so the shared classify_m68k_c4_gap_
  // shapes call below sees the identical retained-fact view the just-run
  // preflight already validated -- never a second, independently
  // reimplemented lookup that could silently drift from it.
  std::set<std::pair<Address, M68kStaticMemoryFactRole>> c4_facts;
  for (const auto &fact : partial.accepted_prefix.static_memory_facts)
    c4_facts.emplace(fact.operation.source.address.value, fact.role);
  std::map<Address, std::pair<Address, std::string_view>> c4_block_stops;
  for (const auto &block : partial.accepted_prefix.static_blocks) {
    for (const auto &provenance : block.instructions) {
      const auto operation = std::find_if(partial.accepted_prefix.ir.begin(), partial.accepted_prefix.ir.end(),
                                          [&](const M68kIrOperation &candidate) {
                                            return same_provenance(candidate.provenance, provenance);
                                          });
      if (operation == partial.accepted_prefix.ir.end())
        return "/* translation rejected: invalid C4 retained prefix */\n";
      const auto shapes = classify_m68k_c4_gap_shapes(*operation, c4_facts, partial.accepted_prefix.mapping_claims);
      // missing_routing is advisory-only in the preflight inventory: the
      // per-instruction body-emission switch below still lowers this
      // shape's own EA through the ordinary runtime genesis_route_access
      // call, so its eventual ROM-read failure remains the existing
      // runtime's GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY defensive
      // stop -- never an emitted GENESIS_STOP_C4_LOWERING_GAP. Keep
      // scanning this block's remaining instructions for an actual stop.
      const auto shape = std::find_if(shapes.begin(), shapes.end(), [](const M68kC4GapShape &candidate) {
        return candidate.gap != M68kC4GapClass::missing_routing;
      });
      if (shape == shapes.end()) continue;
      // ADR-0015 Decision §7: no OTHER catch-all. A represented (ir_kind,
      // gap) combination this codebase has no explicit dimension literal
      // for is not yet a representable C4 lowering-gap shape; fail emission
      // closed rather than report ambiguous evidence for it.
      const auto dimension = c4_lowering_dimension_literal(operation->kind, shape->gap);
      if (!dimension) return "/* translation rejected: unrepresentable C4 lowering gap shape */\n";
      c4_block_stops.emplace(block.id.entry.value, std::make_pair(provenance.source.address.value, *dimension));
      break;
    }
  }
  // SEG-007-T174 correction (see this task's Evidence): whether Tier 2 could
  // EVER engage anywhere in this build is a pure function of
  // `accepted_prefix.validated_code_entry_candidate_roots` -- exactly the
  // same gate `build_genesis_frontier_stop_function` itself checks. When it
  // is empty (the raw/unannotated route: no `--external-hints`, or a hints
  // file with no `code_entry_candidate` records), `build_genesis_frontier_
  // stop_function`'s result is a PURE function of
  // `(accepted_prefix, frontier, sibling_frontier_addresses)` alone,
  // completely independent of whatever `emitted_code_addresses` array is
  // eventually passed -- there is no need to defer its text. This branch
  // computes each frontier's real function text once. Ordinary instruction-
  // boundary resumption now independently uses the final compiled lookup in
  // every build, so lookup declarations are no longer Tier-2-specific. Only when at
  // least one candidate root exists (`tier2_capable`) does this function use
  // the two-pass forward-declare/late-definition shape below, because only
  // then can a frontier's real body legitimately depend on the generation-
  // time `EmittedCodeAddressSet`, which is not known until every accepted
  // block's own reachability is resolved further below.
  const bool tier2_capable = !partial.accepted_prefix.validated_code_entry_candidate_roots.empty() ||
                             !aot_entries->empty();
  std::vector<std::pair<std::uint32_t, std::string>> frontier_stop_functions;  // populated only when !tier2_capable (pre-T174 shape); already-final text
  std::map<Address, std::string> frontier_stop_names;                    // source address -> 8-hex-digit suffix
  // SEG-007-T236: collect call-shaped Tier-2 continuations during the
  // existing validation-only first pass as prospective return authority for
  // Tier-1 closure.  The final pass below still admits each continuation only
  // when it is actually emitted, so this cannot authorize generated RTS code
  // by itself.
  std::set<Address> prospective_tier2_call_continuations;
  for (const auto &frontier : partial.frontiers) {
    std::optional<std::uint32_t> prospective_tier2_call_continuation;
    auto function_text =
        build_genesis_frontier_stop_function(partial.accepted_prefix, frontier, sibling_frontier_addresses, {},
                                             &prospective_tier2_call_continuation);
    if (!function_text || !frontier.diagnostic.provenance) return "/* translation rejected: unrepresentable C4 frontier */\n";
    if (prospective_tier2_call_continuation)
      prospective_tier2_call_continuations.insert(*prospective_tier2_call_continuation);
    const auto address = frontier.diagnostic.provenance->source.address.value;
    std::ostringstream suffix_stream;
    suffix_stream << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << address;
    if (!frontier_stop_names.emplace(address, suffix_stream.str()).second)
      return "/* translation rejected: unrepresentable C4 frontier */\n";  // duplicate source address
    if (!tier2_capable) frontier_stop_functions.emplace_back(address, std::move(*function_text));
  }
  out << header;
  // Keep these pre-existing ordinary-prefix helpers ahead of the compiled-
  // entry declarations. AOT participation changes only whether either helper
  // is needed; exact-PC consistency is computed later from final authority and
  // must not perturb non-AOT generated text ordering.
  const auto aot_emits = [&](M68kIrKind kind) {
    return std::ranges::any_of(*aot_entries, [&](const auto &candidate) {
      return candidate.second->operation.kind == kind;
    });
  };
  const auto emits_mulu_word = std::ranges::any_of(
      partial.accepted_prefix.ir, [](const auto &operation) {
        return operation.kind == M68kIrKind::multiply_unsigned_word;
      }) || aot_emits(M68kIrKind::multiply_unsigned_word);
  const auto emits_muls_word = std::ranges::any_of(
      partial.accepted_prefix.ir, [](const auto &operation) {
        return operation.kind == M68kIrKind::multiply_signed_word;
      }) || aot_emits(M68kIrKind::multiply_signed_word);
  // SEG-022-T003: in a sharded build the helpers and shared typedef/declaration live in the shared header
  // (`static inline`, so a TU that does not use them stays warning-free); the text is otherwise unchanged.
  const bool sharded = sharding_active(out);
  if (sharded) shard_begin_header(out);
  if (emits_mulu_word)
    out << shard_helper_linkage(sharded, "static uint32_t genesis_m68k_mulu_word_cycles(uint16_t source) { uint32_t n = 0U; while (source != 0U) { n += (uint32_t)(source & UINT16_C(1)); source >>= 1U; } return UINT32_C(38) + UINT32_C(2) * n; }\n");
  if (emits_muls_word)
    out << shard_helper_linkage(sharded, "static uint32_t genesis_m68k_muls_word_cycles(uint16_t source) { uint32_t n = 0U; uint32_t bits = ((uint32_t)source) << 1U; for (uint32_t i = 0U; i < 16U; ++i) n += ((bits >> i) ^ (bits >> (i + 1U))) & UINT32_C(1); return UINT32_C(38) + UINT32_C(2) * n; }\n");
  out << "typedef GenesisControlTransfer (*GenesisCompiledEntry)(GenesisRuntime *runtime);\n"
      << (sharded ? "GenesisCompiledEntry genesis_compiled_entry_lookup(uint32_t address);\n"
                  : "static GenesisCompiledEntry genesis_compiled_entry_lookup(uint32_t address);\n");
  if (!sharded)
    out << compact_provenance_helper_declarations << ";\n" << compact_provenance_helper_declarations_2 << ";\n";
  if (sharded) shard_end_header(out);
  if (!tier2_capable) {
    // Every frontier stop function's already-final body text remains in
    // `partial.frontiers` order. No second construction pass is needed when
    // Tier 2 is unavailable.
    for (const auto &[stop_address, function_text] : frontier_stop_functions) {
      ShardUnitScope unit(out, "stop", stop_address, genesis_unit_declaration("genesis_frontier_stop_", stop_address));
      out << function_text;
    }
  } else {
    // SEG-007-T174 / ADR-0024 two-pass shape (only reachable once at least
    // one validated candidate root exists somewhere in this build): forward
    // DECLARATIONS only, in the same `partial.frontiers` order the real
    // bodies (built later, once `EmittedCodeAddressSet` is known) use, so
    // blocks and the dispatcher (built next) may reference these functions
    // by name before their real definitions are appended near the end of
    // this function.
    // (A sharded build declares every stop function in the shared header instead.)
    if (!sharded) for (const auto &frontier : partial.frontiers) {
      const auto address = frontier.diagnostic.provenance->source.address.value;
      out << "static GenesisControlTransfer genesis_frontier_stop_" << frontier_stop_names.at(address)
          << "(GenesisRuntime *runtime);\n";
    }
  }
  // Router failures know their checked address and access shape, but not the
  // statically retained source mapping/fetch. Lower those facts once from the
  // ordinary accepted prefix, keyed by typed source address; this is not a ROM
  // read or a reconstructed host pointer. AOT-only exact-PC mismatch stops
  // attach their one entry's already-validated provenance locally in
  // emit_immutable_rom_aot_body, avoiding a whole-ROM central switch.
  // SEG-022-T003: provenance lookup + static-stop helper form one `meta` unit; both are already external.
  if (sharded) {
    shard_begin_unit(out, "meta", 0U, "void genesis_attach_route_provenance(GenesisRuntimeStop *stop, const GenesisInstructionProvenance *source)");
    shard_declare(out, "GenesisControlTransfer genesis_static_stop(GenesisStopClass class_, GenesisDiagnosticCategory category, const GenesisInstructionProvenance *source, uint8_t has_access, uint32_t address, GenesisAccessWidth width, GenesisAccessDirection direction)");
    shard_declare(out, compact_provenance_helper_declarations);
    shard_declare(out, compact_provenance_helper_declarations_2);
    shard_declare(out, genesis_routed_failure_stop_declaration);
  }
  {
    std::vector<RouteProvenanceRecord> route_records;
    for (const auto &instruction : partial.accepted_prefix.decoded) {
      const auto *mapping =
          select_unique_affine_mapping(partial.accepted_prefix.mapping_claims, instruction.provenance);
      if (mapping == nullptr || mapping->name.size() > genesis_frontier_max_name_length ||
          instruction.raw_bytes.size() > genesis_frontier_max_raw_bytes)
        return "/* translation rejected: invalid C4 retained mapping */\n";
      route_records.push_back({static_cast<std::uint32_t>(instruction.provenance.source.address.value), mapping, &instruction.raw_bytes});
    }
    std::ostringstream route_tables;
    std::ostringstream route_body;
    emit_compact_route_provenance(route_tables, route_body, std::move(route_records));
    out << "\n" << compact_provenance_helper_definitions() << route_tables.str()
        << "void genesis_attach_route_provenance(GenesisRuntimeStop *stop, const GenesisInstructionProvenance *source) {\n"
        << route_body.str();
  }
  out << "}\n"
      << "GenesisControlTransfer genesis_static_stop(GenesisStopClass class_, GenesisDiagnosticCategory category, const GenesisInstructionProvenance *source, uint8_t has_access, uint32_t address, GenesisAccessWidth width, GenesisAccessDirection direction) {\n"
      << "  GenesisControlTransfer transfer = {0}; transfer.kind = GENESIS_STOP; transfer.stop.stop_class = class_; transfer.stop.diagnostic_category = category; transfer.stop.provenance.has_instruction_provenance = 1U; transfer.stop.provenance.instruction = *source;\n"
      << "  if (has_access != 0U) { transfer.stop.provenance.has_access = 1U; transfer.stop.provenance.access_address = address; transfer.stop.provenance.access_width = width; transfer.stop.provenance.access_direction = direction; }\n"
       << "  genesis_attach_route_provenance(&transfer.stop, source); return transfer;\n}\n"
      // SEG-022-T011: the single routed-failure tail shared by every factored AOT routed access. Its statements
      // are exactly the inline failure block `M68kRuntimeCEmitter::routed_read`/`routed_write` and the stack
      // guards emit when not factored (same fields, same order, same provenance helper, same transfer).
      << genesis_routed_failure_stop_declaration << " {\n"
      << "  stop->provenance.has_instruction_provenance = 1U; stop->provenance.instruction = *source; stop->provenance.has_access = 1U; stop->provenance.access_address = address; stop->provenance.access_width = width; stop->provenance.access_direction = direction;\n"
      << "  genesis_attach_route_provenance(stop, source);\n"
      << "  { GenesisControlTransfer transfer = {0}; transfer.kind = GENESIS_STOP; transfer.stop = *stop; return transfer; }\n}\n";
  if (sharded) shard_end_unit(out);
  // A partial program retains real static blocks, not merely a diagnostic.
  // Every static memory fact is checked against the decoded/lifted operation
  // it names before it can select folding or runtime routing below.
  if (partial.accepted_prefix.static_blocks.empty() ||
      partial.accepted_prefix.decoded.size() != partial.accepted_prefix.ir.size())
    return "/* translation rejected: unrepresentable C4 frontier */\n";
  std::map<Address, const M68kDecodedInstruction *> decoded;
  std::map<Address, const M68kIrOperation *> operations;
  for (std::size_t index = 0; index < partial.accepted_prefix.ir.size(); ++index) {
    const auto &instruction = partial.accepted_prefix.decoded[index];
    const auto &operation = partial.accepted_prefix.ir[index];
    if (!independently_decoded_and_lifted(instruction, operation) ||
        !decoded.emplace(instruction.provenance.source.address.value, &instruction).second ||
        !operations.emplace(operation.provenance.source.address.value, &operation).second)
      return "/* translation rejected: invalid C4 prefix */\n";
  }
  for (const auto &[address, entry] : *aot_entries) {
    const auto existing = operations.find(address);
    if (existing != operations.end() && !same_ir(*existing->second, entry->operation))
      return "/* translation rejected: conflicting immutable-ROM AOT identity */\n";
  }
  std::map<std::pair<Address, M68kStaticMemoryFactRole>, const M68kStaticMemoryFact *> facts;
  for (const auto &fact : partial.accepted_prefix.static_memory_facts) {
    const auto instruction = decoded.find(fact.operation.source.address.value);
    if (instruction == decoded.end() || !same_provenance(fact.operation, instruction->second->provenance) ||
        !same_provenance(fact.source_provenance, instruction->second->provenance) ||
        !valid_program_address(fact.address) || !valid_access_width(fact.width) ||
        !valid_access_direction(fact.direction) || fact.width != instruction->second->size)
      return "/* translation rejected: invalid C4 static memory fact */\n";
    const M68kEffectiveAddress *expected_ea = nullptr;
    M68kMemoryAccessDirection expected_direction{};
    switch (instruction->second->kind) {
    case M68kInstructionKind::move:
      if (fact.role == M68kStaticMemoryFactRole::source_read) {
        expected_ea = &instruction->second->source_ea;
        expected_direction = M68kMemoryAccessDirection::read;
      } else if (fact.role == M68kStaticMemoryFactRole::destination_write) {
        expected_ea = &instruction->second->destination_ea;
        expected_direction = M68kMemoryAccessDirection::write;
      }
      break;
    case M68kInstructionKind::movea:
      if (fact.role == M68kStaticMemoryFactRole::source_read) {
        expected_ea = &instruction->second->source_ea;
        expected_direction = M68kMemoryAccessDirection::read;
      }
      break;
    case M68kInstructionKind::tst:
      if (fact.role == M68kStaticMemoryFactRole::source_read) {
        expected_ea = &instruction->second->source_ea;
        expected_direction = M68kMemoryAccessDirection::read;
      }
      break;
    case M68kInstructionKind::clr:
    case M68kInstructionKind::andi:
    case M68kInstructionKind::ori:
    case M68kInstructionKind::eori:
      // SEG-007-T167: ORI/EORI reuse ANDI/CLR's destination-only fact shape;
      // the single destination_write fact authorises the whole RMW destination.
      if (fact.role == M68kStaticMemoryFactRole::destination_write) {
        expected_ea = &instruction->second->destination_ea;
        expected_direction = M68kMemoryAccessDirection::write;
      }
      break;
    case M68kInstructionKind::add:
    case M68kInstructionKind::sub:
    case M68kInstructionKind::logical_and:
    case M68kInstructionKind::logical_or:
    case M68kInstructionKind::eor:
      // SEG-007-T167: non-immediate AND/OR/EOR reuse ADD's source-read /
      // destination read+write RMW fact shape verbatim.
      // SEG-007-T170: SUB shares this exact shape (`<ea>,Dn` or `Dn,<ea>`).
      if (fact.role == M68kStaticMemoryFactRole::source_read) {
        expected_ea = &instruction->second->source_ea;
        expected_direction = M68kMemoryAccessDirection::read;
      } else if (fact.role == M68kStaticMemoryFactRole::destination_read) {
        expected_ea = &instruction->second->destination_ea;
        expected_direction = M68kMemoryAccessDirection::read;
      } else if (fact.role == M68kStaticMemoryFactRole::destination_write) {
        expected_ea = &instruction->second->destination_ea;
        expected_direction = M68kMemoryAccessDirection::write;
      }
      break;
    case M68kInstructionKind::cmpa:
    case M68kInstructionKind::adda:
    case M68kInstructionKind::suba:
    case M68kInstructionKind::cmp:
    case M68kInstructionKind::move_to_sr:   // SEG-021-T018: one word source read
    case M68kInstructionKind::move_to_ccr:  // SEG-021-T018: one word source read
    case M68kInstructionKind::chk:          // SEG-021-T019: one word bound read
      if (fact.role == M68kStaticMemoryFactRole::source_read) {
        expected_ea = &instruction->second->source_ea;
        expected_direction = M68kMemoryAccessDirection::read;
      }
      break;
    case M68kInstructionKind::cmpi:
      // SEG-007-T146: CMPI's immediate source is never a memory fact; its
      // destination is read (never written) to form the CCR-only result.
      if (fact.role == M68kStaticMemoryFactRole::destination_read) {
        expected_ea = &instruction->second->destination_ea;
        expected_direction = M68kMemoryAccessDirection::read;
      }
      break;
    case M68kInstructionKind::subi:
    case M68kInstructionKind::subq:
    case M68kInstructionKind::addq:
    case M68kInstructionKind::addi:
      // SEG-007-T165: SUBQ's quick immediate source is never a memory fact;
      // its destination is a full RMW operand, exactly like ADD's own
      // destination_read/destination_write rows above.
      // SEG-007-T170: SUBI's immediate source is likewise never a memory
      // fact; its destination shares this exact RMW shape.
      // SEG-007-T174 follow-up fix (reviewer-directed, composed-hints
      // regression): ADDQ shares this identical destination-only RMW shape
      // byte-for-byte (mirroring the same gap fixed in this switch's own
      // sibling copies in this file and in machine/genesis/frontend.cpp's
      // retain_fact); this single-frontier emitter's own inline
      // re-verification switch had no case for it either.
      if (fact.role == M68kStaticMemoryFactRole::destination_read) {
        expected_ea = &instruction->second->destination_ea;
        expected_direction = M68kMemoryAccessDirection::read;
      } else if (fact.role == M68kStaticMemoryFactRole::destination_write) {
        expected_ea = &instruction->second->destination_ea;
        expected_direction = M68kMemoryAccessDirection::write;
      }
      break;
    // SEG-021-T009: memory-word shift/rotate: same one-address RMW fact shape as NOT.
    case M68kInstructionKind::shift_rotate:
    // SEG-021-T014: NEG/NEGX are one-address RMW operands with NOT's fact shape.
    case M68kInstructionKind::negate_word:
    case M68kInstructionKind::negate_extended:
    case M68kInstructionKind::negate_decimal:
    case M68kInstructionKind::test_and_set:  // SEG-021-T016
    case M68kInstructionKind::set_conditional:  // SEG-021-T016: memory Scc is read-then-write like TAS
    case M68kInstructionKind::move_from_sr:  // SEG-021-T018: a memory MOVE from SR destination is read then written
    case M68kInstructionKind::not_operand:
      // SEG-007-T168: NOT has no second operand at all (unlike AND/OR/EOR);
      // its sole destination is a full RMW operand needing both
      // destination_read and destination_write, exactly like SUBQ's own
      // destination fact shape immediately above.
      if (fact.role == M68kStaticMemoryFactRole::destination_read) {
        expected_ea = &instruction->second->destination_ea;
        expected_direction = M68kMemoryAccessDirection::read;
      } else if (fact.role == M68kStaticMemoryFactRole::destination_write) {
        expected_ea = &instruction->second->destination_ea;
        expected_direction = M68kMemoryAccessDirection::write;
      }
      break;
    case M68kInstructionKind::btst:
      if (fact.role == M68kStaticMemoryFactRole::destination_read) {
        expected_ea = &instruction->second->destination_ea;
        expected_direction = M68kMemoryAccessDirection::read;
      }
      break;
    case M68kInstructionKind::bchg:
    case M68kInstructionKind::bclr:
    case M68kInstructionKind::bset:
      // SEG-007-T209: unlike BTST (destination_read only, above), BCHG/BCLR/
      // BSET always read-modify-write their destination -- the identical
      // SUBQ/NOT destination_read/destination_write shape above.
      if (fact.role == M68kStaticMemoryFactRole::destination_read) {
        expected_ea = &instruction->second->destination_ea;
        expected_direction = M68kMemoryAccessDirection::read;
      } else if (fact.role == M68kStaticMemoryFactRole::destination_write) {
        expected_ea = &instruction->second->destination_ea;
        expected_direction = M68kMemoryAccessDirection::write;
      }
      break;
    default: break;
    }
    if (expected_ea == nullptr || !m68k_is_statically_foldable_control_ea(*expected_ea) ||
        fact.direction != expected_direction || fact.address.value != m68k_canonical_ea_address(*expected_ea) ||
        !facts.emplace(std::make_pair(fact.operation.source.address.value, fact.role), &fact).second)
      return "/* translation rejected: invalid C4 static memory fact */\n";
    const bool is_ram = m68k_startup_ram_range_in_range(fact.address.value, static_cast<std::uint32_t>(fact.width));
    if (is_ram != (fact.region == M68kAbsoluteOperandRegion::synthetic_work_ram))
      return "/* translation rejected: invalid C4 static memory fact */\n";
    if (!is_ram) {
      if (fact.region == M68kAbsoluteOperandRegion::raw_cartridge_rom) {
        if (fact.direction != M68kMemoryAccessDirection::read)
          return "/* translation rejected: invalid C4 static memory fact */\n";
      } else if (fact.region == M68kAbsoluteOperandRegion::controller_io) {
        // SEG-007-T040: independently re-verify a retained controller_io
        // fact through the exact same shared routing gate that produced it
        // (m68k_route_genesis_device_access) instead of trusting the fact's
        // own region tag -- the identical re-verification discipline this
        // function already applies to every other retained fact field.
        const auto routed = m68k_route_genesis_device_access(
            M68kMemoryAccessRequest{fact.address, fact.width, fact.direction, fact.source_provenance});
        if (fact.direction != M68kMemoryAccessDirection::read ||
            !std::holds_alternative<M68kControllerIoResult>(routed))
          return "/* translation rejected: invalid C4 static memory fact */\n";
      } else if (fact.region == M68kAbsoluteOperandRegion::vdp) {
        // SEG-007-T090 / SEG-007-T113: same independent re-verification
        // through the shared routing gate for a retained VDP-window fact --
        // a WORD read routes to M68kVdpRoutedRead, a WORD/LONG store routes
        // to M68kVdpRoutedWrite; nothing else is a valid VDP fact here.
        const auto routed = m68k_route_genesis_device_access(
            M68kMemoryAccessRequest{fact.address, fact.width, fact.direction, fact.source_provenance});
        const bool valid_vdp_fact =
            (fact.direction == M68kMemoryAccessDirection::read &&
             std::holds_alternative<M68kVdpRoutedRead>(routed)) ||
            (fact.direction == M68kMemoryAccessDirection::write &&
             std::holds_alternative<M68kVdpRoutedWrite>(routed));
        if (!valid_vdp_fact)
          return "/* translation rejected: invalid C4 static memory fact */\n";
      } else if (fact.region == M68kAbsoluteOperandRegion::routed_device) {
        // SEG-007-T115: independent re-verification for a retained Z80
        // bus-arbitration / Z80 program-RAM / PSG routed operand -- the same
        // shared routing gate must still return the generalized routed-device
        // marker for this fact's own direction.
        const auto routed = m68k_route_genesis_device_access(
            M68kMemoryAccessRequest{fact.address, fact.width, fact.direction, fact.source_provenance});
        const auto *routed_access = std::get_if<M68kDeviceRoutedAccess>(&routed);
        if (routed_access == nullptr || routed_access->direction != fact.direction)
          return "/* translation rejected: invalid C4 static memory fact */\n";
      } else {
        return "/* translation rejected: invalid C4 static memory fact */\n";
      }
    }
  }
  // SEG-007-T075: independently re-verify every retained
  // M68kMovemAdjacentLeaFact before consulting it below, exactly like the
  // static-memory-fact loop above -- an invalid/forged fact rejects the
  // whole translation, never a silent fall-back-to-routing for the one
  // instruction it names.
  std::map<Address, const M68kMovemAdjacentLeaFact *> movem_lea_facts;
  for (const auto &fact : partial.accepted_prefix.movem_adjacent_lea_facts) {
    if (!valid_c4_movem_adjacent_lea_fact(fact, decoded, partial.accepted_prefix.static_blocks,
                                          partial.accepted_prefix.static_edges,
                                          partial.accepted_prefix.mapping_claims) ||
        !movem_lea_facts.emplace(fact.consumer.source.address.value, &fact).second)
      return "/* translation rejected: invalid C4 movem adjacent LEA fact */\n";
  }
  for (const auto &[address, instruction] : decoded) {
    // A local C4 cut owns the remainder of its block.  Those instructions
    // never receive semantic bodies, so their unrelated resolver-fact checks
    // must not reject the otherwise valid retained prefix before emission.
    const bool is_c4_truncated = std::any_of(partial.accepted_prefix.static_blocks.begin(),
                                              partial.accepted_prefix.static_blocks.end(),
                                              [&](const M68kStaticBlock &block) {
      const auto cut = c4_block_stops.find(block.id.entry.value);
      if (cut == c4_block_stops.end()) return false;
      const auto instruction_in_block = std::find_if(block.instructions.begin(), block.instructions.end(),
                                                      [&](const auto &provenance) {
                                                        return provenance.source.address.value == address;
                                                      });
      return instruction_in_block != block.instructions.end() && address >= cut->second.first;
    });
    if (is_c4_truncated) continue;
    const auto require_fact = [&](const M68kEffectiveAddress &ea, M68kStaticMemoryFactRole role,
                                  M68kInstructionKind kind) {
      // SEG-007-T065/SEG-007-T068/SEG-007-T070: this consistency check must
      // reject exactly the cases that can never be lowered anywhere in this
      // function -- a statically foldable absolute EA
      // (m68k_is_statically_foldable_control_ea) for which retain_fact
      // declined to record a fact (an ambiguous or unclaimed ROM mapping, a
      // non-RAM write, or a bounds-exceeding raw read; see retain_fact
      // above), and, for `tst`/`clr` only, a predecrement/postincrement EA,
      // which mutates its address register while forming the request; a
      // subsequent router failure would leave that mutation applied with no
      // completed access, an observable, unrecoverable inconsistency this
      // function must reject before any C is emitted (see the dedicated
      // predecrement regression fixture). Every other non-foldable EA
      // (register-direct, plain register-indirect, register-relative
      // displacement, or immediate) never gets a static_memory_facts entry
      // from retain_fact by design, and is not itself a lowering failure
      // here: each instruction kind's own block-emission switch below
      // independently decides whether that EA is representable --
      // write_clr/test_operand/write_move already fall back to runtime
      // routing for a non-mutating non-foldable EA (see the existing
      // `runtime_routing` branches). Requiring a fact for one of these EAs
      // here would reject a case that later check would otherwise accept
      // (or double-reject with a less accurate message), never a case it
      // would otherwise deny.
      //
      // write_move applies this same unconditional gate to both its source
      // and destination EA, exactly like tst/clr apply it to their one
      // operand: SEG-007-T068 removed the narrow audited_ram_move-only
      // exception this gate previously carried, since write_move's own
      // block-emission case (below) now generalizes onto the same
      // resolver-fact/runtime-routing pattern write_clr/test_operand
      // already use, and no longer needs a bespoke pre-check here.
      //
      // SEG-007-T070 (per
      // docs/architecture/c4-move-predecrement-postincrement-commit-
      // contract.md, Q4): the predecrement/postincrement rejection below no
      // longer applies to `move`'s own call site -- write_move's own
      // block-emission case now lowers a predecrement/postincrement operand
      // itself (a deferred-address-commit technique, plus its own narrower
      // same-register aliasing rejection), so this gate defers all
      // representability/aliasing validation for `move` to that case,
      // exactly like every other non-foldable EA already does above. The
      // rejection remains unconditional for `tst`'s and `clr`'s own call
      // sites: fixing their own predecrement/postincrement gap is a
      // separately scoped, separately evidenced follow-on (Non-goals in the
      // decision record above).
      // SEG-007-T070: `move` lowers its own predecrement/postincrement operand.
      // SEG-007-T145: the `add`/`adda` family likewise now lowers an
      // auto-updating operand itself (the add-family deferred-address-commit
      // path; SEG-021-T006 also lowers the ADDA same-register alias), so this gate
      // defers all representability/aliasing validation for those kinds to
      // that path, exactly like `move`. The rejection stays unconditional for
      // `andi`/`btst`, whose own auto-update gap is a separately scoped
      // follow-on (SEG-021-T006 lowers SUBA/CMP/CMPA/CMPI's).
      // SEG-007-T153: `addq` joins `move`/`add`/`adda` here -- ADDQ lowers its
      // own auto-updating destination through the add-family
      // deferred-address-commit path, so this gate defers representability
      // validation for it to that path.
      // SEG-007-T157 / ADR-0019 Stage B: `clr` joins the same list -- an
      // auto-updating CLR destination is now lowered by its own
      // deferred-address-register-commit path (see write_clr's block-
      // emission case below), so this unconditional pre-rejection no longer
      // applies to it either. `andi`/`btst` remain
      // unconditionally rejected here; their own auto-update gap stays a
      // separately scoped follow-on.
      // SEG-007-T192: `movea` joins the same list -- an auto-updating MOVEA
      // source (`-(An)`/`(An)+`) is now lowered by its own
      // deferred-address-register-commit path (see write_movea's
      // block-emission case below), which needs no same-register aliasing
      // decline (unlike ADDA) since MOVEA's destination write never reads
      // the destination register's own prior value. `tst`/`andi`/`suba`/
      // `cmpa`/`btst` remain unconditionally rejected here; their own
      // auto-update gap stays a separately scoped follow-on.
      if (kind != M68kInstructionKind::move && kind != M68kInstructionKind::add &&
          kind != M68kInstructionKind::adda && kind != M68kInstructionKind::addq &&
          kind != M68kInstructionKind::clr && kind != M68kInstructionKind::movea &&
          // SEG-021-T005: `tst` and `not` lower their own auto-updating operand (deferred commit).
          kind != M68kInstructionKind::tst && kind != M68kInstructionKind::not_operand &&
          // SEG-021-T014: NEG/NEGX lower their own auto-updating operand (deferred commit).
          kind != M68kInstructionKind::negate_word && kind != M68kInstructionKind::negate_extended &&
          kind != M68kInstructionKind::negate_decimal &&
          // SEG-021-T016: Scc and TAS lower their own auto-updating operand (deferred commit).
          kind != M68kInstructionKind::set_conditional && kind != M68kInstructionKind::test_and_set &&
          // SEG-021-T007: AND/OR/EOR and ANDI/ORI/EORI lower their own auto-updating operand (deferred commit).
          kind != M68kInstructionKind::logical_and && kind != M68kInstructionKind::logical_or &&
          kind != M68kInstructionKind::eor && kind != M68kInstructionKind::andi &&
          kind != M68kInstructionKind::ori && kind != M68kInstructionKind::eori &&
          // SEG-021-T006: SUBA, CMP, CMPA and CMPI lower their own auto-updating operand (deferred commit).
          kind != M68kInstructionKind::suba && kind != M68kInstructionKind::cmp &&
          kind != M68kInstructionKind::cmpa && kind != M68kInstructionKind::cmpi &&
          // SEG-021-T008: BTST/BCHG/BCLR/BSET lower their own auto-updating destination (deferred commit).
          kind != M68kInstructionKind::btst && kind != M68kInstructionKind::bchg &&
          kind != M68kInstructionKind::bclr && kind != M68kInstructionKind::bset &&
          // SEG-021-T009: memory-word shifts/rotates lower their own auto-updating destination.
          kind != M68kInstructionKind::shift_rotate &&
          // SEG-021-T018: MOVE to SR/CCR and MOVE from SR lower their own auto-updating operand (deferred commit).
          kind != M68kInstructionKind::move_to_sr && kind != M68kInstructionKind::move_to_ccr &&
          kind != M68kInstructionKind::chk &&  // SEG-021-T019: same deferred word-source commit
          kind != M68kInstructionKind::move_from_sr &&
          (ea.mode == M68kEaMode::address_predec || ea.mode == M68kEaMode::address_postinc))
        return false;
      return !m68k_is_statically_foldable_control_ea(ea) || facts.contains({address, role});
    };
    if ((instruction->kind == M68kInstructionKind::move &&
         (!require_fact(instruction->source_ea, M68kStaticMemoryFactRole::source_read, M68kInstructionKind::move) ||
          !require_fact(instruction->destination_ea, M68kStaticMemoryFactRole::destination_write,
                        M68kInstructionKind::move))) ||
        (instruction->kind == M68kInstructionKind::movea &&
         !require_fact(instruction->source_ea, M68kStaticMemoryFactRole::source_read,
                       M68kInstructionKind::movea)) ||
        (instruction->kind == M68kInstructionKind::tst &&
         !require_fact(instruction->source_ea, M68kStaticMemoryFactRole::source_read, M68kInstructionKind::tst)) ||
        (instruction->kind == M68kInstructionKind::clr &&
         !require_fact(instruction->destination_ea, M68kStaticMemoryFactRole::destination_write,
                       M68kInstructionKind::clr)) ||
        // SEG-007-T071: ANDI's destination is required exactly like CLR's --
        // a foldable absolute EA must have a retained fact, and (since `kind`
        // is `andi`, not `move`) a predecrement/postincrement destination is
        // unconditionally rejected here, the same conservative precedent
        // TST/CLR already carry, deferred as a separately scoped follow-on
        // exactly like theirs.
        (instruction->kind == M68kInstructionKind::andi &&
         !require_fact(instruction->destination_ea, M68kStaticMemoryFactRole::destination_write,
                       M68kInstructionKind::andi)) ||
        (instruction->kind == M68kInstructionKind::add &&
         (!require_fact(instruction->source_ea, M68kStaticMemoryFactRole::source_read,
                        M68kInstructionKind::add) ||
          ((instruction->destination_ea.mode != M68kEaMode::data_register &&
            instruction->destination_ea.mode != M68kEaMode::address_register) &&
           (!require_fact(instruction->destination_ea, M68kStaticMemoryFactRole::destination_read,
                          M68kInstructionKind::add) ||
            !require_fact(instruction->destination_ea, M68kStaticMemoryFactRole::destination_write,
                           M68kInstructionKind::add))))) ||
        // SEG-007-T153: ADDQ's destination read-modify-write mirrors ADD's --
        // a foldable absolute destination needs its retained read+write fact;
        // its immediate source never needs one.
        (instruction->kind == M68kInstructionKind::addq &&
         instruction->destination_ea.mode != M68kEaMode::data_register &&
         instruction->destination_ea.mode != M68kEaMode::address_register &&
         (!require_fact(instruction->destination_ea, M68kStaticMemoryFactRole::destination_read,
                        M68kInstructionKind::addq) ||
          !require_fact(instruction->destination_ea, M68kStaticMemoryFactRole::destination_write,
                        M68kInstructionKind::addq))) ||
        // SEG-007-T168: NOT has no second operand at all; its sole
        // destination read-modify-write mirrors ADDQ's own shape exactly.
        (instruction->kind == M68kInstructionKind::not_operand &&
         instruction->destination_ea.mode != M68kEaMode::data_register &&
         instruction->destination_ea.mode != M68kEaMode::address_register &&
         (!require_fact(instruction->destination_ea, M68kStaticMemoryFactRole::destination_read,
                        M68kInstructionKind::not_operand) ||
          !require_fact(instruction->destination_ea, M68kStaticMemoryFactRole::destination_write,
                        M68kInstructionKind::not_operand))) ||
        // SEG-021-T014: NEG/NEGX: one-address RMW like NOT.
        ((instruction->kind == M68kInstructionKind::negate_word ||
          instruction->kind == M68kInstructionKind::negate_extended ||
          instruction->kind == M68kInstructionKind::negate_decimal ||
          instruction->kind == M68kInstructionKind::test_and_set ||  // SEG-021-T016: TAS is one-address RMW too
          instruction->kind == M68kInstructionKind::set_conditional) &&  // memory Scc reads then writes (Dn is register-only)
         instruction->destination_ea.mode != M68kEaMode::data_register &&
         (!require_fact(instruction->destination_ea, M68kStaticMemoryFactRole::destination_read, instruction->kind) ||
          !require_fact(instruction->destination_ea, M68kStaticMemoryFactRole::destination_write,
                        instruction->kind))) ||
        // SEG-021-T009: memory-word shift/rotate: one-address RMW like NOT.
        (instruction->kind == M68kInstructionKind::shift_rotate &&
         instruction->destination_ea.mode != M68kEaMode::data_register &&
         (!require_fact(instruction->destination_ea, M68kStaticMemoryFactRole::destination_read,
                        M68kInstructionKind::shift_rotate) ||
          !require_fact(instruction->destination_ea, M68kStaticMemoryFactRole::destination_write,
                        M68kInstructionKind::shift_rotate))) ||
        // SEG-021-T018: a memory MOVE from SR destination is read then written (memory-Scc shape).
        (instruction->kind == M68kInstructionKind::move_from_sr &&
         instruction->destination_ea.mode != M68kEaMode::data_register &&
         (!require_fact(instruction->destination_ea, M68kStaticMemoryFactRole::destination_read,
                        M68kInstructionKind::move_from_sr) ||
          !require_fact(instruction->destination_ea, M68kStaticMemoryFactRole::destination_write,
                        M68kInstructionKind::move_from_sr))) ||
        ((instruction->kind == M68kInstructionKind::cmpa ||
          instruction->kind == M68kInstructionKind::adda ||
          instruction->kind == M68kInstructionKind::suba ||
          instruction->kind == M68kInstructionKind::cmp ||
          instruction->kind == M68kInstructionKind::move_to_sr ||   // SEG-021-T018
          instruction->kind == M68kInstructionKind::move_to_ccr ||  // SEG-021-T018
          instruction->kind == M68kInstructionKind::chk) &&         // SEG-021-T019
         !require_fact(instruction->source_ea, M68kStaticMemoryFactRole::source_read, instruction->kind)) ||
        // SEG-007-T146: CMPI reads its destination (CCR-only) exactly like BTST.
        (instruction->kind == M68kInstructionKind::cmpi &&
         !require_fact(instruction->destination_ea, M68kStaticMemoryFactRole::destination_read,
                       M68kInstructionKind::cmpi)) ||
        (instruction->kind == M68kInstructionKind::btst &&
         !require_fact(instruction->destination_ea, M68kStaticMemoryFactRole::destination_read,
                       M68kInstructionKind::btst)))
      return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
  }
  std::map<Address, const M68kStaticBlock *> blocks;
  std::set<Address> bound_operations;
  for (const auto &block : partial.accepted_prefix.static_blocks) {
    if (block.id.entry.space != TargetAddressSpace::m68k_program || block.instructions.empty() ||
        block.instructions.front().source.address.value != block.id.entry.value ||
        !blocks.emplace(block.id.entry.value, &block).second)
      return "/* translation rejected: invalid C4 static block entry */\n";
    Address expected = block.id.entry.value;
    for (const auto &instruction : block.instructions) {
      if (!decoded.contains(instruction.source.address.value) ||
          !same_provenance(instruction, decoded.at(instruction.source.address.value)->provenance) ||
          instruction.source.address.value != expected || !bound_operations.insert(expected).second)
        return "/* translation rejected: invalid C4 static block */\n";
      expected = static_cast<Address>(expected + instruction.length.value);
    }
  }
  if (bound_operations.size() != decoded.size() || !partial.accepted_prefix.startup_ingress ||
      !blocks.contains(partial.accepted_prefix.startup_ingress->entry.value))
    return "/* translation rejected: invalid C4 static block */\n";
  // SEG-007-T064: the bounded set of every retained exit's own source
  // address, in place of the single `frontier_address` used before this
  // task -- built from frontier_stop_names' keys, since every entry there
  // already survived build_genesis_frontier_stop_function's own eligibility
  // re-check above. Computed here (moved up from its original post-Tier-1
  // position by SEG-007-T230) because the Tier-1 completeness check below
  // now needs to recognize a genuine typed frontier stop as one of the two
  // legal Tier-1 candidate representations, not only an ordinary block.
  std::set<Address> frontier_addresses;
  for (const auto &[address, suffix] : frontier_stop_names) frontier_addresses.insert(address);
  // SEG-007-T230: a per-source-instruction-address index over every static
  // edge, built once (O(edges)) so the Tier-1 closure walk below can look up
  // a visited block's own outgoing edges in O(1) instead of rescanning the
  // full edge list for every block it visits. Before this task, most
  // closures failed within one or two hops (see the Evidence above), so the
  // O(blocks_in_program x edges_in_program) cost of a full rescan per block
  // never manifested; this task's fix makes many closures now walk their
  // complete reachable subgraph, which made that same pre-existing
  // complexity actually observable (a multi-minute canonical one-shot rerun)
  // until this index replaced it.
  std::map<Address, std::vector<const M68kStaticEdge *>> edges_by_source_for_closure;
  for (const auto &edge : partial.accepted_prefix.static_edges)
    edges_by_source_for_closure[edge.source_instruction.source.address.value].push_back(&edge);
  std::set<Address> retained_instruction_sources_for_closure;
  for (const auto &[entry, block] : blocks) {
    (void)entry;
    for (const auto &instruction : block->instructions)
      retained_instruction_sources_for_closure.insert(instruction.source.address.value);
  }
  // One shared structural predicate is used first with retained sources while
  // deciding Tier-1 closure, then with the final cut-aware emitted source set
  // in the terminal gate below.  Only the assisted runtime-routed profile is
  // widened; an ordinary incoming edge and independently available generic
  // runtime return authority are both mandatory.
  const auto is_runtime_routed_ownerless_return =
      [&](Address entry, const std::set<Address> &represented_sources, bool has_return_authority) {
        return partial.accepted_prefix.offline_inventory_stitch_metrics.offline_candidate_count != 0U &&
               has_return_authority &&
               std::any_of(partial.accepted_prefix.static_edges.begin(),
                           partial.accepted_prefix.static_edges.end(), [&](const M68kStaticEdge &edge) {
          return edge.kind != M68kStaticEdgeKind::return_to_continuation &&
                 edge.target.space == TargetAddressSpace::m68k_program && edge.target.value == entry &&
                 represented_sources.contains(edge.source_instruction.source.address.value);
        });
      };
  const bool prospective_runtime_return_authority =
      !partial.accepted_prefix.static_frames.empty() || !prospective_tier2_call_continuations.empty();
  // C4 closure for every retained ADR-0009 Tier-1 source.  A candidate that
  // was never constructed, was completed as an orphan RTS, or was removed by
  // the accepted-prefix unsafe-edge prune has no representation at all.
  // Stop at the source as a whole in all of those cases; never retain a
  // membership array that can dispatch only the surviving subset.
  std::map<Address, InstructionProvenance> tier1_pre_pc_stops;
  // A candidate is represented when its complete retained C4 successor
  // closure is executable. Checking its entry alone misses the common shape
  // where a later candidate-reachable block lost an unsafe edge during prefix
  // pruning; that later edge is what C4's global structural gate observes.
  //
  // SEG-007-T230 correction (restoring a SEG-007-T124/ADR-0009 regression
  // introduced by SEG-007-T226): this closure walk previously treated two
  // genuinely safe, already-emitted, typed fail-closed shapes as if they
  // were missing representation. Phase 1 evidence (a temporary env-gated
  // trace over the canonical authorized route, reverted before this diff)
  // classified every failing candidate of the runtime-reached Tier-1 source
  // and, program-wide, every other Tier-1 source's failing candidates: 100%
  // of failures were one of exactly two shapes, never a genuinely missing
  // target:
  //   (1) the closure reaches an existing ADR-0015 `c4_block_stops` block
  //       strictly deeper than the candidate's own entry (the candidate's
  //       own entry keeps its unchanged, separately-gated behavior below).
  //       That block already has its own emitted, typed, fail-closed
  //       `genesis_block_stop_*` body; reaching it during execution is a
  //       safe terminal, not a missing one.
  //   (2) the closure walk followed an `indirect_branch`/`indirect_call`
  //       edge -- one candidate member of a SEPARATE, independently
  //       validated Tier-1 relation (this same loop proves, for every
  //       `indirect_target_ea_set` in the program, either complete safe
  //       runtime-membership dispatch or its own typed
  //       `genesis_tier1_indirect_stop_*` pre-PC cut). That relation is
  //       always a safe terminal on its own; treating one of its many
  //       candidate edges as this closure's unconditional next successor
  //       made an outer candidate's completeness depend on the unrelated,
  //       independently-resolved fate of an inner one.
  // SEG-007-T230 performance correction: before this task, most closures
  // failed within one or two hops (see the Evidence above), so a fresh
  // per-candidate DFS was cheap in practice even though it was, in the
  // worst case, O(candidates x retained blocks). This task's correctness
  // fix makes many closures now succeed and walk their complete reachable
  // subgraph, which made that pre-existing worst case fully observable (a
  // multi-minute canonical one-shot rerun) until this memoized, globally
  // linear (O(retained blocks + retained edges) total, not per candidate)
  // rewrite replaced the repeated per-candidate traversal. `downstream_ok`
  // computes a context-independent fact about a single address -- "does
  // this address's own reachable closure, treating any strictly-deeper
  // c4_block_stops node as a safe terminal, stay fully represented" -- and
  // caches it forever once known; `in_progress` breaks a cycle exactly like
  // the previous per-call `visited` set did (a revisited node on the
  // current path is optimistically safe, matching the prior behavior).
  std::unordered_map<Address, bool> downstream_ok_memo;
  std::set<Address> downstream_ok_in_progress;
  std::function<bool(Address)> downstream_ok = [&](Address current) -> bool {
    if (const auto memoized = downstream_ok_memo.find(current); memoized != downstream_ok_memo.end())
      return memoized->second;
    if (!downstream_ok_in_progress.insert(current).second) return true;
    bool result = false;
    const auto block = blocks.find(current);
    if (block == blocks.end()) {
      result = false;
    } else if (c4_block_stops.contains(current)) {
      result = true;
    } else {
      result = true;
      const auto &terminal_instruction = block->second->instructions.back();
      const auto terminal_address = terminal_instruction.source.address.value;
      const auto terminal_edges_entry = edges_by_source_for_closure.find(terminal_address);
      const bool terminal_edges_present = terminal_edges_entry != edges_by_source_for_closure.end() &&
          std::any_of(terminal_edges_entry->second.begin(), terminal_edges_entry->second.end(),
                     [&](const M68kStaticEdge *edge) { return same_provenance(edge->source_instruction, terminal_instruction); });
      if (decoded.at(terminal_address)->kind == M68kInstructionKind::rts &&
          (!terminal_edges_present ||
           std::none_of(terminal_edges_entry->second.begin(), terminal_edges_entry->second.end(),
                         [&](const M68kStaticEdge *edge) {
             return edge->kind == M68kStaticEdgeKind::return_to_continuation &&
                     same_provenance(edge->source_instruction, terminal_instruction);
           })) &&
          !is_runtime_routed_ownerless_return(current, retained_instruction_sources_for_closure,
                                              prospective_runtime_return_authority)) {
        result = false;
      } else if (terminal_edges_present) {
        for (const auto *edge : terminal_edges_entry->second) {
          if (!same_provenance(edge->source_instruction, terminal_instruction)) continue;
          if (edge->kind == M68kStaticEdgeKind::indirect_branch || edge->kind == M68kStaticEdgeKind::indirect_call)
            continue;
          if (blocks.contains(edge->target.value)) {
            if (!downstream_ok(edge->target.value)) { result = false; break; }
          } else if (!sibling_frontier_addresses.contains(edge->target.value)) {
            result = false;
            break;
          }
        }
      }
    }
    downstream_ok_in_progress.erase(current);
    downstream_ok_memo.emplace(current, result);
    return result;
  };
  // The candidate's own entry keeps its unchanged, stricter behavior (a
  // c4_block_stops node AT the entry is still rejected, exactly as before
  // this task); only a c4_block_stops node strictly deeper in the closure
  // (handled inside the memoized `downstream_ok` above) is now a safe
  // terminal.
  const auto candidate_has_executable_closure = [&](Address entry) {
    const auto block = blocks.find(entry);
    if (block == blocks.end() || c4_block_stops.contains(entry)) return false;
    return downstream_ok(entry);
  };
  for (const auto &target_set : partial.accepted_prefix.indirect_target_ea_sets) {
    const auto source = target_set.source_instruction.source.address.value;
    const auto decoded_source = decoded.find(source);
    const auto operation = operations.find(source);
    const auto source_block = std::find_if(partial.accepted_prefix.static_blocks.begin(),
                                           partial.accepted_prefix.static_blocks.end(),
                                           [&](const M68kStaticBlock &block) {
      return !block.instructions.empty() &&
             same_provenance(block.instructions.back(), target_set.source_instruction);
    });
    if (decoded_source == decoded.end() || operation == operations.end() ||
        source_block == partial.accepted_prefix.static_blocks.end() ||
        !same_provenance(decoded_source->second->provenance, target_set.source_instruction) ||
        !same_ea(target_set.control_ea, decoded_source->second->source_ea) ||
        target_set.candidates.empty())
      return "/* translation rejected: invalid C4 indirect target set */\n";
    // SEG-007-T230 / ADR-0009: Tier-1 completeness is complete generated
    // REPRESENTATION -- an ordinary emitted block with a safe closure, OR a
    // genuine existing typed frontier stop (never bare semantic-partition-
    // boundary membership, which `frontier_addresses` structurally excludes
    // -- it is built only from `frontier_stop_names`, each entry of which
    // already survived `build_genesis_frontier_stop_function`'s own
    // eligibility re-check). This restores the pre-T226 admissible-edge-
    // target rule for Tier-1 candidates specifically, while keeping T226's
    // atomicity intact: every candidate must still be represented one way
    // or the other, or the whole source becomes a `tier1_pre_pc_stops` cut
    // below -- there is still no subset dispatch.
    bool every_candidate_represented = true;
    for (std::size_t index = 0; index < target_set.candidates.size(); ++index) {
      const auto candidate = target_set.candidates[index];
      if (candidate.space != TargetAddressSpace::m68k_program ||
          (index != 0U && !less(target_set.candidates[index - 1U], candidate)))
        return "/* translation rejected: invalid C4 indirect target set */\n";
      const bool candidate_ordinary_block =
          blocks.contains(candidate.value) && candidate_has_executable_closure(candidate.value);
      const bool candidate_frontier_stop = frontier_addresses.contains(candidate.value);
      every_candidate_represented = every_candidate_represented &&
          (candidate_ordinary_block || candidate_frontier_stop);
    }
    if (!every_candidate_represented &&
        !tier1_pre_pc_stops.emplace(source, target_set.source_instruction).second)
      return "/* translation rejected: invalid C4 indirect target set */\n";
  }
  // SEG-007-T243 / ADR-0039 ("successor freeze 2"): a block whose terminal
  // is a computed-control JMP/JSR (`pc_index8`, or a bare register-indirect
  // `(An)` control EA) but for which discovery proved NEITHER a finite
  // Tier-1 `M68kIndirectTargetEaSet` NOR recorded a Tier-2-eligible
  // `M68kUnprovenIndirectControlEaSet` fact used to be pruned wholesale by
  // `build_analysis` (platforms/genesis/machine/src/frontend.cpp)'s own
  // `unresolved_computed_control` erase branch -- a non-monotonic loss of
  // representation for an otherwise decoded, complete, independently-safe
  // block. That erasure is removed; the block now survives into
  // `accepted_prefix.static_blocks` with its terminal instruction still
  // fully retained. Route it through the exact same `tier1_pre_pc_stops`
  // cut mechanism the incomplete-Tier-1-candidate-set case immediately
  // above already uses: the block keeps its own ordinary entry, body,
  // dispatcher arm, and `EmittedCodeAddressSet` membership (this is not a
  // frontier exclusion -- see the per-instruction body-emission loop below,
  // which defers to this source's own typed `genesis_tier1_indirect_stop_*`
  // fail-closed stop -- reusing the identical
  // GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET /
  // GENESIS_DIAG_REACHED_UNRESOLVED_DIRECT_EDGE diagnostic pair the
  // incomplete-Tier-1 case already emits, no new runtime diagnostic -- the
  // moment its own body-emission reaches this terminal, strictly BEFORE any
  // architectural mutation for it). A source that already carries a
  // `M68kUnprovenIndirectControlEaSet` fact is deliberately excluded here:
  // by ADR-0024's own construction (see `compute_candidate_frontier_
  // addresses`'s documented invariant in build_analysis), such a source's
  // address is already excluded from ever becoming a retained block
  // terminal in the first place and is already routed through Tier 2's own
  // `emit_m68k_compiled_address_existence_check` path -- this new class
  // therefore only ever matches the strictly narrower "no fact at all"
  // residual T194 previously described, never a source with an existing
  // Tier-1 or Tier-2 representation.
  for (const auto &block : partial.accepted_prefix.static_blocks) {
    if (block.instructions.empty()) continue;
    const auto &block_terminal = block.instructions.back();
    const auto terminal_decoded_it = decoded.find(block_terminal.source.address.value);
    if (terminal_decoded_it == decoded.end() ||
        !same_provenance(terminal_decoded_it->second->provenance, block_terminal) ||
        (terminal_decoded_it->second->kind != M68kInstructionKind::jmp &&
         terminal_decoded_it->second->kind != M68kInstructionKind::jsr))
      continue;
    const auto &block_terminal_ea = terminal_decoded_it->second->source_ea;
    const bool block_terminal_is_computed_control_ea =
        block_terminal_ea.mode == M68kEaMode::pc_index8 ||
        (block_terminal_ea.mode == M68kEaMode::address_indirect && block_terminal_ea.displacement == 0 &&
         block_terminal_ea.extension_words == 0U);
    if (!block_terminal_is_computed_control_ea) continue;
    const bool has_proven_target_set =
        std::any_of(partial.accepted_prefix.indirect_target_ea_sets.begin(),
                    partial.accepted_prefix.indirect_target_ea_sets.end(),
                    [&](const M68kIndirectTargetEaSet &set) {
          return set.source_instruction.source.address.value == block_terminal.source.address.value;
        });
    if (has_proven_target_set) continue;
    const bool has_unproven_tier2_fact =
        std::any_of(partial.accepted_prefix.unproven_indirect_control_ea_sets.begin(),
                    partial.accepted_prefix.unproven_indirect_control_ea_sets.end(),
                    [&](const M68kUnprovenIndirectControlEaSet &set) {
          return set.source_instruction.source.address.value == block_terminal.source.address.value;
        });
    if (has_unproven_tier2_fact) continue;
    tier1_pre_pc_stops.emplace(block_terminal.source.address.value, block_terminal);
  }
  std::map<Address, std::vector<const M68kStaticEdge *>> edges_by_source;
  for (const auto &edge : partial.accepted_prefix.static_edges) {
    const auto source = edge.source_instruction.source.address;
    if (source.space != TargetAddressSpace::m68k_program || edge.target.space != TargetAddressSpace::m68k_program ||
        !decoded.contains(source.value) || !same_provenance(edge.source_instruction, decoded.at(source.value)->provenance) ||
         (edge.kind != M68kStaticEdgeKind::direct_branch && edge.kind != M68kStaticEdgeKind::fallthrough &&
         // SEG-007-T181 / ADR-0027: the synthesized partition-boundary
         // adjacency edge -- accepted exactly like `fallthrough`; the emission
         // reachability walk below keeps its target a retained emitted block
         // (ADR-0014 P1 entry-connectivity) and no branch/jump is emitted at it.
         edge.kind != M68kStaticEdgeKind::fallthrough_continuation &&
         edge.kind != M68kStaticEdgeKind::direct_call && edge.kind != M68kStaticEdgeKind::return_to_continuation &&
         // SEG-007-T124 / ADR-0009: one candidate member of a proven
         // multi-candidate indirect target set (see the per-block indirect
         // validation below, which requires the complete retained set to
         // equal exactly this source's own `M68kIndirectTargetEaSet`).
         edge.kind != M68kStaticEdgeKind::indirect_branch && edge.kind != M68kStaticEdgeKind::indirect_call))
      return "/* translation rejected: C4 edge target is not static or the retained frontier */\n";
    edges_by_source[source.value].push_back(&edge);
  }
  // ADR-0015 Q1: a cut block contributes no outgoing program-control edge.
  // Emit only blocks still reachable from the ingress through uncut retained
  // edges; a C4 stop is a terminal sink, never a dispatcher target.
  // SEG-007-T142 correction (adversarial validation): this reachability walk
  // must use an index-based worklist, not a single-pass std::set iterator --
  // a std::set is ordered, so a newly discovered lower-address target
  // inserted "behind" an in-progress iterator would never be revisited and
  // would be silently dropped from emission. This mirrors the already-correct
  // pattern above at `reachable_addresses` / `pending`.
  std::set<Address> emitted_block_entries{partial.accepted_prefix.startup_ingress->entry.value};
  std::vector<Address> emitted_block_pending{partial.accepted_prefix.startup_ingress->entry.value};
  // SEG-007-T047 / ADR-0020 §6: the build-resolved IRQ6 autovector handler is a
  // second, entry-disconnected reachability root -- its handler blocks are
  // reached only when generated-runtime interrupt admission overrides
  // runtime->pc. Seed the emission walk from it so the handler blocks are
  // emitted and get their own genesis_dispatch arms; every edge it follows is
  // an already-validated retained C4 edge. genesis.cpp only writes
  // runtime->irq6_handler_present when this handler entry is also an emitted
  // block, so an un-emitted handler leaves the mechanism inert.
  if (partial.accepted_prefix.irq6_handler_entry &&
      partial.accepted_prefix.irq6_handler_entry->space == TargetAddressSpace::m68k_program &&
      blocks.contains(partial.accepted_prefix.irq6_handler_entry->value) &&
      emitted_block_entries.insert(partial.accepted_prefix.irq6_handler_entry->value).second)
    emitted_block_pending.push_back(partial.accepted_prefix.irq6_handler_entry->value);
  // SEG-007-T222 / ADR-0037: the vector-5 handler is the same kind of
  // entry-disconnected reachability root, mirroring `irq6_handler_entry`
  // exactly.
  if (partial.accepted_prefix.divide_by_zero_handler_entry &&
      partial.accepted_prefix.divide_by_zero_handler_entry->space == TargetAddressSpace::m68k_program &&
      blocks.contains(partial.accepted_prefix.divide_by_zero_handler_entry->value) &&
      emitted_block_entries.insert(partial.accepted_prefix.divide_by_zero_handler_entry->value).second)
    emitted_block_pending.push_back(partial.accepted_prefix.divide_by_zero_handler_entry->value);
  // SEG-021-T018 / ADR 0043: the vector-8 handler is the same kind of root.
  if (partial.accepted_prefix.privilege_violation_handler_entry &&
      partial.accepted_prefix.privilege_violation_handler_entry->space == TargetAddressSpace::m68k_program &&
      blocks.contains(partial.accepted_prefix.privilege_violation_handler_entry->value) &&
      emitted_block_entries.insert(partial.accepted_prefix.privilege_violation_handler_entry->value).second)
    emitted_block_pending.push_back(partial.accepted_prefix.privilege_violation_handler_entry->value);
  // SEG-021-T019: every software-exception vector handler is the same kind of root.
  for (const auto &[software_vector, handler] : partial.accepted_prefix.software_exception_handler_entries)
    if (handler.space == TargetAddressSpace::m68k_program && blocks.contains(handler.value) &&
        emitted_block_entries.insert(handler.value).second)
      emitted_block_pending.push_back(handler.value);
  // SEG-007-T174 / ADR-0024: every validated external code-entry candidate
  // root is a second kind of entry-disconnected reachability root, exactly
  // like the IRQ6 handler above -- seed the emission walk from each one (only
  // when it is actually a retained block in this accepted prefix) so its own
  // block, and every block it reaches through its own retained edges, gets a
  // real `genesis_block_<addr>` function and a `genesis_dispatch` arm, and
  // therefore becomes a genuine member of `EmittedCodeAddressSet` below.
  for (const auto &root : partial.accepted_prefix.validated_code_entry_candidate_roots)
    if (root.space == TargetAddressSpace::m68k_program && blocks.contains(root.value) &&
        emitted_block_entries.insert(root.value).second)
      emitted_block_pending.push_back(root.value);
  // A complete Tier-1 relation is dispatched to every one of its represented
  // candidates: an ordinary ADR-0009-proven block, seeded here (including
  // lifecycle roots the ingress traversal cannot reach directly), or a
  // genuine typed frontier stop, which already has its own `genesis_dispatch`
  // arm from `frontier_stop_names` and is deliberately never added to
  // `emitted_block_entries` -- that set is Tier 2's own exact-membership
  // authority (ADR-0024) and must contain only real retained blocks.
  // Incomplete relations were recorded above as source stops and
  // deliberately seed nothing.
  for (const auto &target_set : partial.accepted_prefix.indirect_target_ea_sets)
    if (!tier1_pre_pc_stops.contains(target_set.source_instruction.source.address.value))
      for (const auto &candidate : target_set.candidates)
        if (blocks.contains(candidate.value) && emitted_block_entries.insert(candidate.value).second)
          emitted_block_pending.push_back(candidate.value);
  // SEG-007-T185 / ADR-0028 §8.2 (direct-control target preservation): every
  // validated direct-control edge target `T` that is already a retained C4 block
  // (`blocks.contains(T)`) is a third kind of entry-disconnected reachability
  // root. A large one-shot multi-unit aggregate can retain the callee block and
  // every call site that targets it, yet leave the callee unreachable through
  // the static edge walk alone (its caller reaches it only via a Tier-2 runtime
  // dispatch, not a followed static edge) -- which previously left the callee
  // out of `EmittedCodeAddressSet`, so a generated direct `BSR`/`JSR`/branch to
  // it hit `genesis_internal_dispatch_inconsistency_stop` at runtime. Seeding
  // the walk from each such target (only when it is genuinely a retained block)
  // is strictly additive: it can only ADD an already-validated retained block to
  // the emitted set, never remove one and never weaken the fail-closed sentinel
  // for a target genuinely absent from `blocks`. Every edge here already passed
  // the structural per-edge target gate above. Gated on a non-empty offline
  // inventory so the raw/unannotated single-unit route stays byte-identical:
  // that route's static edge walk from the ingress already covers every retained
  // direct-control target, and T183/T184 additions carry the same guard.
  //
  // SEG-007-T185 (H2): this is the emitter-stage equivalent of the analysis
  // pass's `!analysis.semantic_partition_boundary_addresses.empty()` gate
  // (platforms/genesis/machine/src/frontend.cpp). Both are false only on the
  // raw/unannotated single-unit route and true on the offline-inventory route.
  // This site uses the recorded stitch metric because it is the stable, in-scope
  // "was this a multi-unit route" fact at emission time; the analysis site names
  // the boundary set directly because that is the membership it protects. Both
  // guard strictly-additive work, so the two forms are interchangeable in effect.
  if (partial.accepted_prefix.offline_inventory_stitch_metrics.offline_candidate_count != 0U) {
    for (const auto &source_entry : edges_by_source)
      for (const auto *edge : source_entry.second)
        if ((edge->kind == M68kStaticEdgeKind::direct_call ||
             edge->kind == M68kStaticEdgeKind::direct_branch ||
             edge->kind == M68kStaticEdgeKind::return_to_continuation) &&
            blocks.contains(edge->target.value) &&
            emitted_block_entries.insert(edge->target.value).second)
          emitted_block_pending.push_back(edge->target.value);
  }
  // SEG-007-T243 correction (second adversarial review of PR #386): ADR-0039's
  // design point is that a block's own RETENTION in `accepted_prefix.
  // static_blocks` -- not provable static reachability from this curated,
  // finite list of runtime-entry roots -- is the executable-existence
  // authority for a block with NO retained static predecessor at all. T242
  // already removed root/graph reachability as a RETENTION gate
  // (`runtime_frontier_eligible`'s BFS); without this step, EMISSION still
  // silently re-imposed the identical reachability requirement one layer
  // downstream for exactly that shape: a block whose sole would-be
  // predecessor was entirely ERASED from `blocks` during analysis (T242's
  // own scenario -- e.g. the predecessor's own unrelated unsafe outgoing
  // edge) leaves the downstream block with literally no discoverable
  // incoming edge anywhere in this function's reachability data, even
  // though it is independently decoded, lifted, bound, mapped, aligned,
  // non-conflicting, complete, lowerable, and safe (`blocks` above already
  // re-validates every one of those facts unconditionally). Before this
  // correction that block silently received NO representation at all here:
  // no body, no `genesis_dispatch` arm, no `EmittedCodeAddressSet`
  // membership, not even a typed fail-closed stop -- exactly the "safe
  // represented code excluded merely because static closure/root ownership
  // was absent" category ADR-0039 forbids.
  //
  // This is a strictly NARROWER fix than unconditionally seeding every
  // block in `blocks` (an earlier, overly broad attempt regressed
  // `genesis_startup_runtime_c4_test`'s own
  // "A cut in a static block that Q1 prunes downstream of the first cut has
  // no retained emitted caller, so its static stop function must not be
  // emitted" contract): a block downstream of a legitimate ADR-0015 Q1 cut
  // (a `c4_block_stops`/`tier1_pre_pc_stops` terminal sink) DOES have a
  // retained static predecessor -- that predecessor block is genuinely
  // present in `blocks`, just intentionally non-continuing by architectural
  // design -- and must keep deferring to the existing cut-aware walk below,
  // which correctly excludes it (the walk's own `c4_block_stops.contains(
  // entry) || tier1_pre_pc_stops.contains(source)) continue;` guard stops
  // there). Only a block with NO retained predecessor at all -- neither a
  // live one reached by the walk NOR a cut one deliberately excluded by it
  // -- is unconditionally its own emission root here: there is nothing left
  // for graph reachability alone to prove as a soundness requirement for
  // that shape once retention itself no longer requires it.
  //
  // SEG-007-T243 correction (fourth adversarial review of PR #386): the
  // zero-predecessor rule above is necessary but not sufficient. A retained
  // component can be root-disconnected while every one of its members still
  // has an internal retained predecessor -- e.g. a mutual two-block cycle
  // A -> B, B -> A, where both A and B are independently walked, valid,
  // complete, safe, and retained, but neither satisfies "no retained
  // predecessor" (each is the other's). Left uncorrected, such a component
  // is invisible to every existing seed (it is not a curated root, not a
  // Tier-1 candidate, not zero-predecessor, and not downstream of any cut
  // either) -- graph topology alone would remain the executable-existence
  // authority for exactly this shape, which ADR-0039 forbids just as much
  // as the single-block case above.
  //
  // A prior attempt at this fix wrongly concluded that ANY component with
  // two or more members can never be "purely downstream of a cut" merely
  // because its own internal edges are non-cut. That is false: consider
  // `root -> C`, `C --CUT--> A` (C is a legitimate, retained
  // `c4_block_stops`/`tier1_pre_pc_stops` non-continuing sink), `A -> B`,
  // `B -> A`. Once cut-sourced edges are excluded from the live graph, {A,
  // B} still forms its own two-member component via their mutual live edge
  // -- but A and B have NO independent executable ingress at all: their
  // ONLY retained incoming edge from anywhere in the program is the
  // intentional, non-continuing cut at C. A component's own internal
  // mutual connectivity says nothing about whether the component as a
  // WHOLE has any path into it that isn't itself already excluded; seeding
  // it here would let a component "launder" a cut's own intentional
  // non-continuation into unconditional executable authority -- exactly
  // the ADR-0015 Q1 violation this task must not introduce, just for a
  // multi-block shape instead of the already-covered singleton one.
  //
  // The corrected rule: build the "live" retained-block graph using only
  // edges whose SOURCE is not itself a cut terminal (a `c4_block_stops`/
  // `tier1_pre_pc_stops` entry never propagates further, exactly like the
  // existing walk's own cut-aware guard), compute its weakly-connected
  // components over every block in `blocks`, and seed one deterministic
  // representative (its lowest address) from a component with two or more
  // members ONLY when the component genuinely has no external retained
  // ingress that is cut-sourced (and is not already covered by an
  // independent authorized root, `validated_code_entry_candidate_roots`).
  // Since a genuine live (non-cut) edge between two blocks always merges
  // them into the SAME component (by the very union this loop performs),
  // the ONLY kind of external incoming edge a component can still have,
  // after this union completes, is either a cut-sourced one or none at
  // all -- there is no third case to reason about. A component with zero
  // external retained incoming edges (T242's own "sole predecessor
  // entirely erased" scenario, generalized to more than one hop) is
  // genuinely independent and eligible; a component with at least one
  // cut-sourced external incoming edge (the counterexample above) is not,
  // unless one of its own members is independently a validated candidate
  // root (in which case it already has its own authorized identity,
  // unrelated to the cut). Seeding is idempotent
  // (`emitted_block_entries.insert(...).second`), so redundantly seeding a
  // component that the curated roots or the zero-predecessor rule already
  // reached is a harmless no-op; the existing cut-aware walk below still
  // performs all further propagation and still respects every cut exactly
  // as before -- this block only ever adds new WORKLIST entries, it never
  // adds, removes, or reinterprets an edge, root, or cut.
  {
    std::set<Address> block_terminal_sources;
    for (const auto &[terminal_entry, terminal_block] : blocks)
      block_terminal_sources.insert(terminal_block->instructions.back().source.address.value);
    std::set<Address> targets_with_a_retained_predecessor;
    for (const auto &edge : partial.accepted_prefix.static_edges)
      if (block_terminal_sources.contains(edge.source_instruction.source.address.value))
        targets_with_a_retained_predecessor.insert(edge.target.value);
    for (const auto &[entry, block] : blocks) {
      (void)block;
      if (targets_with_a_retained_predecessor.contains(entry)) continue;
      if (emitted_block_entries.insert(entry).second) emitted_block_pending.push_back(entry);
    }
    // Deterministic union-find over every block entry in `blocks`, unioning
    // only along "live" (non-cut-sourced) retained edges.
    std::map<Address, Address> component_parent;
    for (const auto &[entry, block] : blocks) {
      (void)block;
      component_parent.emplace(entry, entry);
    }
    std::function<Address(Address)> component_find = [&](Address value) -> Address {
      while (component_parent.at(value) != value) {
        component_parent[value] = component_parent.at(component_parent.at(value));
        value = component_parent.at(value);
      }
      return value;
    };
    const auto component_union = [&](Address left, Address right) {
      const auto left_root = component_find(left);
      const auto right_root = component_find(right);
      if (left_root == right_root) return;
      // Deterministic tie-break (lower address wins as the new root) so
      // repeated generation is byte-identical regardless of edge order.
      if (left_root < right_root) component_parent[right_root] = left_root;
      else component_parent[left_root] = right_root;
    };
    for (const auto &edge : partial.accepted_prefix.static_edges) {
      const auto source_address = edge.source_instruction.source.address.value;
      if (!block_terminal_sources.contains(source_address) || !blocks.contains(edge.target.value)) continue;
      const bool source_is_cut_terminal =
          tier1_pre_pc_stops.contains(source_address) ||
          std::any_of(c4_block_stops.begin(), c4_block_stops.end(),
                      [&](const auto &stop) { return stop.second.first == source_address; });
      if (source_is_cut_terminal) continue;
      // The edge's own SOURCE block entry is the block whose terminal
      // instruction address equals `source_address`; every `blocks` entry
      // is keyed by its own block entry, and blocks are bound 1:1 to their
      // own terminal instruction by the `blocks`/`bound_operations`
      // construction above, so a direct scan over `blocks` here is exact.
      for (const auto &[entry, block] : blocks)
        if (block->instructions.back().source.address.value == source_address)
          component_union(entry, edge.target.value);
    }
    std::map<Address, std::vector<Address>> components_by_root;
    for (const auto &[entry, block] : blocks) {
      (void)block;
      components_by_root[component_find(entry)].push_back(entry);
    }
    // Map each block entry to the component root it belongs to, so a
    // component's own external-ingress check can classify an edge's target
    // as "inside" or "outside" the component in O(1).
    std::map<Address, Address> component_of;
    for (const auto &[root, members] : components_by_root)
      for (const auto member : members) component_of.emplace(member, root);
    std::set<Address> components_with_cut_external_predecessor;
    for (const auto &edge : partial.accepted_prefix.static_edges) {
      const auto source_address = edge.source_instruction.source.address.value;
      if (!block_terminal_sources.contains(source_address) || !blocks.contains(edge.target.value)) continue;
      const bool source_is_cut_terminal =
          tier1_pre_pc_stops.contains(source_address) ||
          std::any_of(c4_block_stops.begin(), c4_block_stops.end(),
                      [&](const auto &stop) { return stop.second.first == source_address; });
      if (!source_is_cut_terminal) continue;
      // A cut-sourced edge, by construction, never took part in the union
      // above, so it may legitimately cross a component boundary. Find the
      // SOURCE block's own component and compare it to the TARGET's.
      Address source_entry{};
      bool found_source_entry = false;
      for (const auto &[entry, block] : blocks)
        if (block->instructions.back().source.address.value == source_address) {
          source_entry = entry;
          found_source_entry = true;
          break;
        }
      if (!found_source_entry) continue;
      const auto source_component = component_of.find(source_entry);
      const auto target_component = component_of.find(edge.target.value);
      if (source_component == component_of.end() || target_component == component_of.end()) continue;
      if (source_component->second != target_component->second)
        components_with_cut_external_predecessor.insert(target_component->second);
    }
    for (auto &[root, members] : components_by_root) {
      if (members.size() < 2U) continue;
      const bool has_own_authorized_root =
          std::any_of(members.begin(), members.end(), [&](Address member) {
        return std::any_of(partial.accepted_prefix.validated_code_entry_candidate_roots.begin(),
                            partial.accepted_prefix.validated_code_entry_candidate_roots.end(),
                            [&](const M68kProgramAddress &candidate_root) {
              return candidate_root.space == TargetAddressSpace::m68k_program && candidate_root.value == member;
            });
      });
      if (!has_own_authorized_root && components_with_cut_external_predecessor.contains(root)) continue;
      const auto representative = *std::min_element(members.begin(), members.end());
      if (emitted_block_entries.insert(representative).second) emitted_block_pending.push_back(representative);
    }
  }
  for (std::size_t index = 0; index < emitted_block_pending.size(); ++index) {
    const auto entry = emitted_block_pending[index];
    const auto block = blocks.find(entry);
    if (block == blocks.end()) return "/* translation rejected: invalid C4 static block */\n";
    const auto source = block->second->instructions.back().source.address.value;
    if (c4_block_stops.contains(entry) || tier1_pre_pc_stops.contains(source)) continue;
    const auto edges = edges_by_source.find(source);
    if (edges == edges_by_source.end()) continue;
    for (const auto *edge : edges->second)
      if (blocks.contains(edge->target.value) &&
           emitted_block_entries.insert(edge->target.value).second)
        emitted_block_pending.push_back(edge->target.value);
  }
  // Source pre-PC stops are ordinary terminal cuts.  Validate control edges
  // only after the cut-aware reachability walk: an edge from code made dead by
  // its owning source stop cannot reach the dispatcher, while every separately
  // rooted or otherwise live block remains subject to the same fail-closed
  // target-representation rule.  Semantic-boundary membership is deliberately
  // not a root or a representation.
  std::set<Address> emitted_instruction_sources;
  for (const auto entry : emitted_block_entries) {
    const auto block = blocks.find(entry);
    if (block == blocks.end()) return "/* translation rejected: invalid C4 static block */\n";
    for (const auto &instruction : block->second->instructions)
      emitted_instruction_sources.insert(instruction.source.address.value);
  }
  for (const auto &edge : partial.accepted_prefix.static_edges) {
    const auto source = edge.source_instruction.source.address.value;
    const bool source_is_c4_cut = std::any_of(
        c4_block_stops.begin(), c4_block_stops.end(), [&](const auto &stop) { return stop.second.first == source; });
    if (!emitted_instruction_sources.contains(source) || tier1_pre_pc_stops.contains(source) || source_is_c4_cut)
      continue;
    if (!blocks.contains(edge.target.value) && !frontier_addresses.contains(edge.target.value))
      return "/* translation rejected: C4 edge target is not static or the retained frontier */\n";
  }
  // The final compiled-address owner maps every resumable ordinary instruction
  // boundary to its existing block function.  Interrupt retirement may expose
  // any N+1 PC inside a block; retaining only block entries would make RTE
  // replay the block prefix or fail dispatch.  Cuts remain terminal: only the
  // source boundary at the cut is retained, never instructions after it.
  std::map<Address, Address> ordinary_compiled_owners;
  for (const auto entry : emitted_block_entries) {
    const auto block = blocks.find(entry);
    if (block == blocks.end()) return "/* translation rejected: invalid C4 static block */\n";
    const auto cut = c4_block_stops.find(entry);
    for (const auto &instruction : block->second->instructions) {
      const auto address = instruction.source.address.value;
      if (!ordinary_compiled_owners.emplace(address, entry).second)
        return "/* translation rejected: conflicting ordinary compiled address owner */\n";
      if (tier1_pre_pc_stops.contains(address) ||
          (cut != c4_block_stops.end() && cut->second.first == address))
        break;
    }
  }
  // This one sorted set is the authority for lookup, Tier-2 membership, and
  // frontier precedence.  An ordinary/AOT collision is legal only when the
  // already-validated operation identity (including provenance) agrees; the
  // retained ordinary owner then wins deterministically.
  std::set<Address> emitted_code_addresses;
  for (const auto &[address, owner] : ordinary_compiled_owners) {
    (void)owner;
    emitted_code_addresses.insert(address);
  }
  for (const auto &[address, entry] : *aot_entries) {
    const auto ordinary = ordinary_compiled_owners.find(address);
    if (ordinary != ordinary_compiled_owners.end()) {
      const auto operation = operations.find(address);
      if (operation == operations.end() || !same_ir(*operation->second, entry->operation))
        return "/* translation rejected: conflicting immutable-ROM AOT identity */\n";
    }
    emitted_code_addresses.insert(address);
  }
  // SEG-021-T026: AOT enumeration runs after discovery/ADR-0038, so its exact
  // PC assignments were previously invisible to retention. Derive the first
  // real divergence from the final compiled/frontier authority, not from a
  // second graph: represented targets dispatch normally; each absent exact
  // target becomes a source-provenanced post-retirement typed frontier in its
  // producer body. Dynamic-control membership guards remain unchanged.
  std::set<Address> represented_exact_pcs = frontier_addresses;
  represented_exact_pcs.insert(emitted_code_addresses.begin(), emitted_code_addresses.end());
  const auto aot_unrepresented_exact_pcs_result =
      immutable_rom_aot_unrepresented_exact_pcs(*aot_entries, represented_exact_pcs);
  if (!aot_unrepresented_exact_pcs_result)
    return "/* translation rejected: immutable-ROM AOT PC effect or frontier provenance has no consistency owner */\n";
  auto aot_unrepresented_exact_pcs = *aot_unrepresented_exact_pcs_result;
  const std::vector<std::uint32_t> emitted_code_address_set(emitted_code_addresses.begin(),
                                                             emitted_code_addresses.end());
  // SEG-007-T181 / ADR-0027 §6 (wiring the deferred ADR-0026 §5 metrics): once
  // the EmittedCodeAddressSet is final, emit one normalized stderr line with the
  // final emitted block count and emitted code-address count. Numbers only.
  // Guarded on a non-empty offline inventory so the raw/unannotated route stays
  // byte-identical and silent. tools/genesis_startup_bridge.py parses this line
  // and merges the two counts into offline_inventory_stitch_metrics.
  if (partial.accepted_prefix.offline_inventory_stitch_metrics.offline_candidate_count != 0U)
    std::fprintf(stderr,
                 "segarecomp: offline inventory emission: emitted_block_count=%zu "
                 "emitted_code_address_count=%zu\n",
                 emitted_block_entries.size(), emitted_code_address_set.size());
  // SEG-007-T174 / ADR-0024: now that `EmittedCodeAddressSet` is known, build
  // every frontier stop function's REAL definition (already forward-declared
  // above) and append it to `out`. Re-running the same validated construction
  // a second time, now with the real array, must succeed identically to the
  // first (nullopt-free) pass above for every frontier -- a difference here
  // would mean the two passes disagree about a purely deterministic function
  // of already-fixed inputs, which is an internal inconsistency, not a normal
  // rejection path. This second pass only runs at all when `tier2_capable`;
  // the raw/unannotated route already emitted its final, complete function
  // bodies in the single pre-T174-shaped pass above and must not be touched
  // again here (`build_genesis_frontier_stop_function`'s own Tier-2 gate is
  // otherwise unreachable in that case anyway, but skipping the whole loop
  // keeps the generated-text ordering identical to the pre-T174 shape, not
  // merely semantically equivalent).
  // SEG-007-T208 correction (ADR-0011 Decision §1 / ADR-0024's literal
  // call-shaped "unframed Tier-2 call" case): every call-shaped Tier-2 site's
  // own genuine, statically-known continuation, collected alongside its real
  // function body below and unioned into `runtime_return_target_set` further
  // down -- restricted to a continuation that is itself a member of the final
  // `emitted_code_addresses` (ordinary block entry/instruction boundary or an
  // admitted immutable-ROM AOT identity; every block entry is a member;
  // SEG-021-T030), never fabricated and never dependent on which downstream target
  // the site's own runtime-computed EA happens to resolve to.
  std::set<Address> tier2_call_continuations;
  if (tier2_capable) {
    for (const auto &frontier : partial.frontiers) {
      std::optional<std::uint32_t> tier2_call_continuation;
      auto function_text = build_genesis_frontier_stop_function(partial.accepted_prefix, frontier,
                                                                  sibling_frontier_addresses, emitted_code_address_set,
                                                                  &tier2_call_continuation);
      if (!function_text) return "/* translation rejected: unrepresentable C4 frontier */\n";
      {
        const auto stop_address = frontier.diagnostic.provenance->source.address.value;
        ShardUnitScope unit(out, "stop", stop_address, genesis_unit_declaration("genesis_frontier_stop_", stop_address));
        out << *function_text;
      }
      if (tier2_call_continuation && emitted_code_addresses.contains(*tier2_call_continuation))
        tier2_call_continuations.insert(*tier2_call_continuation);
    }
  }
  // Select C4 sinks only after Q1 reachability has removed blocks downstream
  // of a cut.  A static-only cut has no retained caller, so defining its
  // otherwise unreachable static function would violate strict C11's unused
  // function diagnostic without contributing to the emitted program.
  for (auto stop = c4_block_stops.begin(); stop != c4_block_stops.end();) {
    if (!emitted_block_entries.contains(stop->first))
      stop = c4_block_stops.erase(stop);
    else
      ++stop;
  }
  for (auto stop = tier1_pre_pc_stops.begin(); stop != tier1_pre_pc_stops.end();) {
    const auto block = std::find_if(partial.accepted_prefix.static_blocks.begin(),
                                    partial.accepted_prefix.static_blocks.end(),
                                    [&](const M68kStaticBlock &candidate) {
      return !candidate.instructions.empty() &&
             candidate.instructions.back().source.address.value == stop->first;
    });
    if (block == partial.accepted_prefix.static_blocks.end() ||
        !emitted_block_entries.contains(block->id.entry.value))
      stop = tier1_pre_pc_stops.erase(stop);
    else
      ++stop;
  }
  for (const auto &[source, provenance] : tier1_pre_pc_stops) {
    // Every exit of this iteration emits exactly the one `genesis_tier1_indirect_stop_<source>` function.
    ShardUnitScope tier1_stop_unit(out, "stop", source, genesis_unit_declaration("genesis_tier1_indirect_stop_", source));
    // SEG-021-T028: a retained JMP/JSR terminal whose Tier-1 set (or Tier-1
    // fact) cannot be lowered but whose EA is Tier-2 eligible gets exactly one
    // owner, the existing Tier-2 emitted-set membership lowering (same function
    // name and call site as the typed stop it replaces). A call-shaped site
    // additionally contributes its genuine continuation (T208's exact
    // non-fabrication rule) and only where return authority already exists
    // (`prospective_runtime_return_authority`), so the RTS emission decisions
    // made earlier from that flag stay valid.
    if (tier2_capable) {
      const auto decoded_terminal = decoded.find(source);
      if (decoded_terminal != decoded.end() &&
          (decoded_terminal->second->kind == M68kInstructionKind::jmp ||
           (decoded_terminal->second->kind == M68kInstructionKind::jsr && prospective_runtime_return_authority))) {
        const bool is_call = decoded_terminal->second->kind == M68kInstructionKind::jsr;
        std::ostringstream tier2_name;
        tier2_name << "genesis_tier1_indirect_stop_" << std::uppercase << std::hex << std::setw(8)
                   << std::setfill('0') << source;
        std::optional<std::uint32_t> pre_pc_call_continuation;
        if (auto tier2_text = build_tier2_computed_control_function(
                partial.accepted_prefix, provenance, decoded_terminal->second->source_ea, is_call,
                tier2_name.str(), emitted_code_address_set, &pre_pc_call_continuation)) {
          out << *tier2_text;
          if (pre_pc_call_continuation && emitted_code_addresses.contains(*pre_pc_call_continuation))
            tier2_call_continuations.insert(*pre_pc_call_continuation);
          continue;
        }
      }
    }
    out << build_genesis_tier1_indirect_pre_pc_stop(provenance);
  }
  for (const auto &[entry, stop] : c4_block_stops) {
    (void)entry;
    const auto operation = operations.find(stop.first);
    if (operation == operations.end()) return "/* translation rejected: invalid C4 retained prefix */\n";
    std::ostringstream suffix;
    suffix << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << stop.first;
    ShardUnitScope c4_stop_unit(out, "stop", stop.first, genesis_unit_declaration("genesis_c4_lowering_stop_", stop.first));
    out << "static GenesisControlTransfer genesis_c4_lowering_stop_" << suffix.str()
        << "(GenesisRuntime *runtime) {\n"
        << "  GenesisInstructionProvenance source = {0};\n"
        << "  GenesisControlTransfer transfer = {0};\n"
        << "  (void)runtime;\n"
        << "  source.cpu_variant = GENESIS_CPU_MC68000;\n"
        << "  source.source_address = UINT32_C(" << hex(stop.first, 8) << ");\n"
        << "  source.image_offset = UINT64_C(" << operation->second->provenance.source.image_offset.value << ");\n"
        << "  source.primary_bytes[0] = UINT8_C(" << hex(operation->second->provenance.bytes[0], 2) << ");\n"
        << "  source.primary_bytes[1] = UINT8_C(" << hex(operation->second->provenance.bytes[1], 2) << ");\n"
        << "  source.length = UINT32_C(" << operation->second->provenance.length.value << ");\n"
        << "  transfer = genesis_static_stop(GENESIS_STOP_C4_LOWERING_GAP, GENESIS_DIAG_C4_LOWERING_GAP, &source, 0U, 0U, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_READ);\n"
        << "  transfer.stop.c4_lowering_dimensions = " << stop.second << ";\n"
        << "  return transfer;\n"
        << "}\n";
  }
  // SEG-007-T134 correction (ADR-0011 Decision §1): a call identity owns
  // every `return_to_continuation` edge validly bound to it -- not exactly
  // one, and not only when its callee's own entry block directly terminates
  // in RTS. That narrower "exactly one, only for a direct-entry-block RTS"
  // requirement was a leftover single-instruction-callee assumption, not a
  // soundness requirement: this loop still requires frame identity
  // uniqueness (`frame_count == 1`); per-edge structural validity for
  // whichever `return_to_continuation` edges ARE present is enforced by the
  // per-terminal edge switch below (source is genuinely an RTS at exactly
  // that terminal, target equals exactly this call's own continuation, and a
  // uniquely matching retained frame exists); and the actual runtime
  // membership check (return_from_subroutine emission below) no longer
  // depends on edges at all -- it validates the popped target against the
  // whole-program set of every retained frame's own proven continuation.
  // `synthesize_return_edges` (static_discovery.cpp) already represents
  // every legitimate exit RTS of a callee containing an internal branch, or
  // exiting through more than one RTS, correctly; this loop must not reject
  // that truthful representation.
  for (const auto &frame : partial.accepted_prefix.static_frames) {
    const auto frame_count = std::count_if(partial.accepted_prefix.static_frames.begin(),
                                           partial.accepted_prefix.static_frames.end(),
                                           [&](const M68kStaticFrame &other) {
      return same_call(other.call, frame.call);
    });
    if (frame_count != 1U) return "/* translation rejected: invalid C4 static return mapping */\n";
  }
  // SEG-007-T134 correction (ADR-0011 Decision §1): the deterministic,
  // deduplicated whole-program set of every retained call's own proven
  // continuation address -- computed exactly once, used identically by
  // EVERY retained `return_from_subroutine` terminal below. This is the
  // literal reading of ADR-0011 Decision §1: every generated RTS membership
  // check tests the actual popped 68000 stack value against this one shared
  // set, never against only the `return_to_continuation` edges sourced at
  // that particular RTS. `std::set<Address>` iteration is already sorted,
  // so this is deterministic on repeat and across independent runs.
  std::set<Address> runtime_return_target_set;
  for (const auto &frame : partial.accepted_prefix.static_frames)
    runtime_return_target_set.insert(frame.call.continuation.value);
  // SEG-007-T208 correction (ADR-0011 Decision §1 / ADR-0024's literal
  // call-shaped "unframed Tier-2 call" case): union in every call-shaped
  // Tier-2 site's own genuine continuation (collected above, already
  // restricted to a real emitted/dispatchable block entry) -- Tier-2
  // admission never constructs an ordinary discovery-time `M68kStaticCall`
  // frame for any site (ADR-0024: Tier 2 is realized entirely at C4
  // EMISSION time, outside discovery's own call/frame graph), so this
  // continuation would otherwise never reach `runtime_return_target_set` at
  // all even though it is exactly as statically known as any ordinary call's
  // own continuation. This never requires knowing which downstream routine
  // any Tier-2 site actually reaches at runtime -- only the call
  // instruction's own fixed (source address + length) successor, which is
  // independent of its target.
  for (const auto &continuation : tier2_call_continuations) runtime_return_target_set.insert(continuation);
  // SEG-007-T246 correction (ADR-0011 Decision §1 / SEG-007-T208's own
  // precedent immediately above, applied with T208's own exact non-
  // fabrication condition, never a weaker one): union in every admitted
  // immutable-ROM-AOT `call_general`/`bsr_call` candidate's own genuine
  // continuation too, but ONLY when that continuation is ITSELF already a
  // member of `emitted_code_addresses` -- the same final, already-computed
  // whole-program compiled/dispatchable identity set (every resumable
  // ordinary instruction boundary AND every admitted AOT identity, unioned above at this
  // function's own `emitted_code_addresses` construction) T208's own
  // `emitted_block_entries.contains(*tier2_call_continuation)` check
  // already requires for its analogous Tier-2 case. This is deliberately
  // T208's own exact "independently retained/represented" condition,
  // literally extended (never weakened) to also accept an AOT identity as
  // a valid representation of the continuation, exactly as T244 already
  // established an AOT identity needs no CFG root/edge/frame/call/return
  // fact to be a genuine, dispatchable program identity in its own right.
  // Admitting the CALL instruction itself never required its continuation
  // to be represented (T244 deliberately removed any CFG-reachability
  // requirement for AOT admission); this is a strictly narrower, separate
  // question -- whether that continuation may additionally become RTS
  // return-target AUTHORITY -- and it is intentionally still gated exactly
  // like T208's own case: an unrepresented successor byte sequence merely
  // FOLLOWING an admitted call never joins this set merely because the
  // call has call semantics, and a call whose continuation is
  // unrepresented may still be validly admitted and dispatched through
  // its own semantic contract (pushing that continuation and jumping to
  // its callee) -- it simply contributes no RTS authority for it. This
  // never requires knowing which downstream routine the call reaches --
  // only the call instruction's own fixed (source address + length)
  // successor, already proven safe to compute per-candidate by the
  // AOT-safety predicate's own `call_general`/`bsr_call` carve-out -- and
  // it classifies as an intrinsic semantic property of an already-
  // represented call instruction whose successor is also independently
  // represented, never static reachability, a fabricated CFG call edge, a
  // frame, or a guessed return owner.
  for (const auto &[aot_address, aot_entry] : *aot_entries)
    if ((aot_entry->operation.kind == M68kIrKind::call_general ||
         aot_entry->operation.kind == M68kIrKind::bsr_call)) {
      const auto continuation = aot_address + aot_entry->decoded.provenance.length.value;
      if (emitted_code_addresses.contains(continuation)) runtime_return_target_set.insert(continuation);
    }
  for (const auto &block : partial.accepted_prefix.static_blocks) {
    const auto &terminal = block.instructions.back();
    const auto terminal_operation = operations.at(terminal.source.address.value);
    for (std::size_t index = 0; index + 1U < block.instructions.size(); ++index)
      if (edges_by_source.contains(block.instructions[index].source.address.value))
        return "/* translation rejected: nonterminal C4 static edge */\n";
    // An orphan target is deliberately not an ordinary dispatch entry.  Its
    // owning Tier-1 source has already become a pre-PC stop, so validating or
    // lowering the unreachable RTS would turn a safe source stop back into a
    // whole-program rejection.
    if (!emitted_block_entries.contains(block.id.entry.value)) continue;
    const bool is_branch = terminal_operation->kind == M68kIrKind::general_branch ||
                           terminal_operation->kind == M68kIrKind::dbcc_loop;
    const auto *terminal_decoded = decoded.at(terminal.source.address.value);
    // SEG-007-T124 / ADR-0009: a JSR whose decoded source EA is the brief
    // PC-relative indexed form is a distinct, multi-candidate terminal shape
    // -- never overloaded onto the single-target direct-call/direct-branch
    // representation below.  Both JSR and JMP use the same proved target-set
    // and runtime membership seam; only JSR owns candidate-specific frames.
    // SEG-007-T178 / ADR-0009 producer extension: pure address-register-
    // indirect (`JMP (An)` / `JSR (An)`, mode 2, no displacement/index/
    // extension word) joins the brief PC-relative indexed form as a
    // multi-candidate computed control terminal.
    const auto &tsea = terminal_decoded->source_ea;
    const bool terminal_is_computed_control_ea =
        tsea.mode == M68kEaMode::pc_index8 ||
        (tsea.mode == M68kEaMode::address_indirect && tsea.displacement == 0 && tsea.extension_words == 0U);
    const bool is_indirect_call = terminal_operation->kind == M68kIrKind::call_general &&
                                   terminal_is_computed_control_ea;
    const bool is_indirect_branch = terminal_operation->kind == M68kIrKind::jump_general &&
                                    terminal_is_computed_control_ea;
    // SEG-007-T185: a direct (statically-folded absolute.w / absolute.l /
    // d16(PC)) `JMP` terminal. Its `direct_branch` static edge, its
    // `jump_general` IR/effect (`effect.pc == direct_target`), and its
    // block-entry target are all pre-existing, decoder/lifter/discovery-
    // supported facts already validated for this terminal below; C4's
    // per-terminal control-transfer classification simply never recognized it
    // (only `general_branch`/`dbcc_loop`), so a retained block ending in a
    // direct JMP hit the "lacks terminal control transfer" sentinel. Handled
    // exactly like an unconditional `general_branch`: one direct edge, no
    // fallthrough, `emit_m68k_operation_c` sets `runtime->pc` to the folded
    // target and the existing block-tail GENESIS_CONTINUE_AT_PC reaches the
    // successor through the unchanged genesis_dispatch. No new edge kind, no
    // new control-transfer mechanism, no new CPU form.
    const bool is_direct_jump =
        terminal_operation->kind == M68kIrKind::jump_general && !terminal_is_computed_control_ea;
    const bool is_call = (terminal_operation->kind == M68kIrKind::call_general ||
                          terminal_operation->kind == M68kIrKind::bsr_call) && !is_indirect_call;
    const bool is_return = terminal_operation->kind == M68kIrKind::return_from_subroutine;
    // SEG-007-T047 / ADR-0020 §9: RTE is a terminal control transfer with no
    // static successors (the restored PC is a generated-runtime fact popped
    // from the exception frame). It carries no direct/fallthrough/call/return
    // static edge and needs no return-target membership check.
    // SEG-021-T019: RTR (PC popped from its CCR/PC frame) and the instruction-word exceptions (ILLEGAL, line
    // 1010/1111, every other illegal word: control enters the build-time-rooted vector handler) are terminals of
    // the same no-static-successor shape.
    const bool is_exception_return = terminal_operation->kind == M68kIrKind::return_from_exception ||
                                     terminal_operation->kind == M68kIrKind::return_restore_condition_codes ||
                                     terminal_operation->kind == M68kIrKind::instruction_exception;
    // SEG-007-T174 / ADR-0024: Tier 2 (the shared `EmittedCodeAddressSet`
    // dispatch mechanism) never reaches this per-BLOCK terminal-validation
    // path at all -- a Tier-2-eligible site is, by construction, always the
    // FAILING frontier instruction itself, which `exclude_frontier_
    // instruction` (platforms/genesis/machine/src/frontend.cpp's `build_analysis`)
    // always excludes from ever becoming part of a retained static block.
    // Tier 2 is realized entirely inside `build_genesis_frontier_stop_
    // function` instead, which replaces the generic always-stop frontier
    // function with a Tier-2-aware one when a matching
    // `M68kUnprovenIndirectControlEaSet` fact exists for that exact
    // frontier's own diagnostic provenance. This per-block path therefore
    // stays exactly ADR-0009's own Tier-1-only shape, unchanged.
    const M68kIndirectTargetEaSet *indirect_target_set = nullptr;
    // SEG-007-T243 / ADR-0039 ("successor freeze 2"): a terminal already
    // routed through `tier1_pre_pc_stops` (see its construction below --
    // both the pre-existing incomplete-Tier-1-candidate-set case and the
    // new no-proof-at-all case this task adds) has no
    // `M68kIndirectTargetEaSet` to validate here by construction; the
    // per-instruction body-emission loop further below returns through its
    // own typed `genesis_tier1_indirect_stop_*` stop before ever reaching
    // this terminal's operation lowering. Skip this whole indirect-target
    // validation block for it instead of dereferencing a null
    // `indirect_target_set`.
    if ((is_indirect_call || is_indirect_branch) &&
        !tier1_pre_pc_stops.contains(terminal.source.address.value)) {
      for (const auto &candidate_set : partial.accepted_prefix.indirect_target_ea_sets) {
        if (!same_provenance(candidate_set.source_instruction, terminal_decoded->provenance)) continue;
        if (indirect_target_set != nullptr) return "/* translation rejected: invalid C4 indirect target set */\n";
        indirect_target_set = &candidate_set;
      }
      if (indirect_target_set == nullptr || indirect_target_set->candidates.empty() ||
          !same_ea(indirect_target_set->control_ea, terminal_decoded->source_ea))
        return "/* translation rejected: invalid C4 indirect target set */\n";
      for (std::size_t index = 0; index < indirect_target_set->candidates.size(); ++index) {
        // A Tier-1 proof establishes legal targets, not permission to emit a
        // subset dispatcher. The closure above either seeded every candidate
        // as a represented block/frontier or cut this source to its typed
        // pre-PC stop. SEG-007-T230: a candidate frontier IS one of the two
        // legal representations now (matching the completeness check
        // above) -- a bare semantic-partition-boundary membership is not,
        // and remains rejected here exactly as before.
        const auto candidate_value = indirect_target_set->candidates[index].value;
        if (!tier1_pre_pc_stops.contains(terminal.source.address.value) &&
            (!blocks.contains(candidate_value) || c4_block_stops.contains(candidate_value)) &&
            !frontier_addresses.contains(candidate_value))
          return "/* translation rejected: invalid C4 indirect target set */\n";
        if (index > 0U && !less(indirect_target_set->candidates[index - 1U], indirect_target_set->candidates[index]))
          return "/* translation rejected: invalid C4 indirect target set */\n";
      }
    }
    // SEG-007-T040 (C3): a block may also legitimately end with no
    // control-transfer instruction at all, when discovery's own
    // build_analysis fix (see discover_m68k_general_startup) promoted a
    // straight-line block whose terminal instruction's ordinary PC advance
    // lands exactly on the retained frontier -- the exact minimal
    // one-instruction-block shape this checkpoint closes. This is not a new
    // control-transfer kind and introduces no new PC-effect owner: it is
    // recognized here purely from the block's own already-validated static
    // edge shape (its one fallthrough edge, sourced from this terminal
    // instruction), and reuses the exact same runtime->pc tail-check every
    // other block below already emits -- the terminal instruction's own
    // emitted C (e.g. TST via emit_m68k_operation_c) already advances
    // runtime->pc by its own length like any other non-terminal instruction
    // in the block.
    // SEG-007-T066: the real Sonic ROM reached a second, previously
    // unrepresented shape of this same no-control-transfer case: the one
    // fallthrough edge's target is not the retained frontier, but another
    // block already retained in this same accepted prefix (`blocks`). This
    // introduces no second control-transfer mechanism and no new PC-effect
    // owner either: `genesis_dispatch` (below) already checks
    // `runtime->pc` through the final compiled-address lookup on
    // every `GENESIS_CONTINUE_AT_PC` result via `genesis_runtime_run`'s
    // own dispatch loop (tools/genesis_startup_bridge_runtime.c), the exact
    // same mechanism a branch/call/return-terminated block already relies on
    // to reach its own successor block. A straight-line fallthrough to a
    // retained block's entry is therefore already mechanically
    // representable by the existing typed dispatcher with no code-generation
    // change here at all; only this check's own prior narrowness (accepting
    // a fallthrough target that is a frontier, but not one that is a
    // retained block entry) prevented it from being recognized.
    const auto &terminal_edges = edges_by_source[terminal.source.address.value];
    // SEG-007-T181 / ADR-0027: a fallthrough_continuation edge is the
    // address-order block adjacency at a synthesized partition boundary --
    // treated here exactly like a plain `fallthrough` (no branch/jump emitted,
    // the existing runtime->pc tail-check reaches the successor block).
    const bool is_straight_line_fallthrough =
        !is_branch && !is_direct_jump && !is_call && !is_return && !is_exception_return && !is_indirect_call && !is_indirect_branch && terminal_edges.size() == 1U &&
        (terminal_edges.front()->kind == M68kStaticEdgeKind::fallthrough ||
         terminal_edges.front()->kind == M68kStaticEdgeKind::fallthrough_continuation) &&
        (frontier_addresses.contains(terminal_edges.front()->target.value) ||
          blocks.contains(terminal_edges.front()->target.value));
    const bool is_cut_block = c4_block_stops.contains(block.id.entry.value) ||
                              tier1_pre_pc_stops.contains(terminal.source.address.value);
    if (!is_branch && !is_direct_jump && !is_call && !is_return && !is_exception_return && !is_indirect_call && !is_indirect_branch && !is_straight_line_fallthrough && !is_cut_block)
      return "/* translation rejected: C4 block lacks terminal control transfer */\n";
    const auto effect = m68k_operation_effect(*terminal_operation);
    if ((is_branch || is_direct_jump || is_call) && effect.pc != M68kPcEffectKind::direct_target)
      return "/* translation rejected: unsupported C4 operation */\n";
    std::size_t direct_count{};
    std::size_t fallthrough_count{};
    std::size_t continuation_count{};  // SEG-007-T181 / ADR-0027
    std::size_t call_count{};
    std::size_t return_count{};
    // SEG-007-T134 correction (second pass): the exact-count completeness
    // check this terminal used to enforce ("callee entry block == RTS
    // block" => exactly one return edge) also incidentally bounded
    // `return_count` from above. Removing that narrow assumption without
    // replacement let a duplicated/forged `return_to_continuation` edge --
    // same call identity, same target, same source, appearing twice in
    // `static_edges` -- silently pass. Every legitimate
    // `return_to_continuation` edge at one terminal must name a distinct
    // call identity (a shared RTS returns to N distinct calls through N
    // distinct call identities, never the same call identity twice); this
    // tracks the call identities already seen at THIS terminal and rejects
    // a repeat.
    std::vector<M68kStaticCall> return_edge_calls;
    std::vector<Address> reached_indirect_targets;
    // SEG-007-T064: a block's terminal instruction has at most two outgoing
    // edges (fallthrough + taken, or a single unconditional transfer), so at
    // most two distinct frontier addresses can be reached from one block's
    // tail; each is its own genesis_frontier_stop_<addr> comparison line
    // below, in place of the single `reaches_frontier` boolean used before
    // this task.
    std::set<Address> reached_frontier_addresses;
    for (const auto *edge : edges_by_source[terminal.source.address.value]) {
      if (edge->kind == M68kStaticEdgeKind::direct_branch) {
        ++direct_count;
        if (edge->target.value != effect.direct_target) return "/* translation rejected: C4 branch target disagrees with static edge */\n";
      } else if (edge->kind == M68kStaticEdgeKind::fallthrough) {
        ++fallthrough_count;
        if (edge->target.value != terminal.source.address.value + terminal.length.value)
          return "/* translation rejected: C4 fallthrough disagrees with static edge */\n";
      } else if (edge->kind == M68kStaticEdgeKind::fallthrough_continuation) {
        // SEG-007-T181 / ADR-0027: the synthesized partition-boundary adjacency
        // edge. Its target is exactly the predecessor instruction's next PC
        // (sequential fallthrough or branch/call continuation) -- validated like
        // a plain `fallthrough`; it emits no branch/jump.
        ++continuation_count;
        if (edge->target.value != terminal.source.address.value + terminal.length.value)
          return "/* translation rejected: C4 fallthrough_continuation disagrees with static edge */\n";
      } else if (edge->kind == M68kStaticEdgeKind::direct_call) {
        ++call_count;
        if (!edge->call || !same_provenance(edge->source_instruction, edge->call->caller) ||
            edge->target.value != effect.direct_target || !same(edge->target, edge->call->callee) ||
            std::count_if(partial.accepted_prefix.static_frames.begin(), partial.accepted_prefix.static_frames.end(),
                          [&](const M68kStaticFrame &frame) { return same_call(frame.call, *edge->call); }) != 1)
          return "/* translation rejected: invalid C4 static call edge */\n";
      } else if (edge->kind == M68kStaticEdgeKind::return_to_continuation) {
        // SEG-007-T134 correction (ADR-0011 Decision §1): this terminal's
        // RTS is no longer required to be exactly its call's own callee-
        // entry-block terminal ("edge->call->callee.value == block.id.entry.value")
        // -- a leftover single-instruction-callee assumption. A callee
        // containing an internal branch before its RTS legitimately exits
        // through an RTS whose own block is NOT its entry block;
        // `synthesize_return_edges` (static_discovery.cpp) already proves
        // that RTS is genuinely reachable from that call's own callee entry
        // before ever emitting this edge. The remaining checks (source is
        // genuinely this terminal, target is exactly this call's own
        // continuation, and a uniquely matching retained frame exists) are
        // unchanged and still fully validate the edge.
        ++return_count;
        if (!edge->call || !same(edge->target, edge->call->continuation) ||
            !same_provenance(edge->source_instruction, terminal) ||
            std::count_if(partial.accepted_prefix.static_frames.begin(), partial.accepted_prefix.static_frames.end(),
                          [&](const M68kStaticFrame &frame) { return same_call(frame.call, *edge->call); }) != 1 ||
            std::any_of(return_edge_calls.begin(), return_edge_calls.end(),
                        [&](const M68kStaticCall &seen) { return same_call(seen, *edge->call); }))
          return "/* translation rejected: invalid C4 static return edge */\n";
        return_edge_calls.push_back(*edge->call);
      } else if (edge->kind == M68kStaticEdgeKind::indirect_call) {
        // SEG-007-T124 / ADR-0009: one candidate-specific call identity per
        // member of the retained `M68kIndirectTargetEaSet` (never
        // overloaded onto `direct_call`).
        if (!is_indirect_call || !edge->call || !same_provenance(edge->source_instruction, edge->call->caller) ||
            !same_provenance(edge->source_instruction, terminal_decoded->provenance) ||
            !same(edge->target, edge->call->callee) ||
            edge->call->continuation.value != terminal.source.address.value + terminal.length.value ||
            (!tier1_pre_pc_stops.contains(terminal.source.address.value) &&
             std::count_if(partial.accepted_prefix.static_frames.begin(), partial.accepted_prefix.static_frames.end(),
                           [&](const M68kStaticFrame &frame) { return same_call(frame.call, *edge->call); }) != 1))
          return "/* translation rejected: invalid C4 indirect call edge */\n";
        reached_indirect_targets.push_back(edge->target.value);
      } else if (edge->kind == M68kStaticEdgeKind::indirect_branch) {
        if (!is_indirect_branch || edge->call || !same_provenance(edge->source_instruction, terminal_decoded->provenance))
          return "/* translation rejected: invalid C4 indirect branch edge */\n";
        reached_indirect_targets.push_back(edge->target.value);
      } else return "/* translation rejected: invalid C4 static edge */\n";
      // SEG-007-T250 / ADR-0039: a retained predecessor edge whose target is
      // BOTH a typed static frontier AND already a member of the final
      // `emitted_code_addresses` (built above: every resumable ordinary
      // instruction boundary plus every admitted immutable-ROM AOT identity) must not directly
      // invoke that target's `genesis_frontier_stop_<addr>` here -- doing so
      // would erase an otherwise valid independently compiled executable
      // representation for the same architectural PC, which ADR-0039
      // forbids. Excluding it from `reached_frontier_addresses` lets this
      // terminal fall through to the generic `GENESIS_CONTINUE_AT_PC` tail
      // below instead, so the common `genesis_dispatch` compiled-entry
      // lookup (the sole runtime selection mechanism) picks up the already-
      // compiled body. A frontier target that is NOT yet in
      // `emitted_code_addresses` is completely unaffected and keeps stopping
      // exactly as before -- this changes representation-precedence
      // arbitration only, never static-analysis truth.
      if (frontier_addresses.contains(edge->target.value) && !emitted_code_addresses.contains(edge->target.value))
        reached_frontier_addresses.insert(edge->target.value);
    }
    // SEG-007-T243: mirrors the guard above -- a `tier1_pre_pc_stops` cut
    // terminal never has any `indirect_call`/`indirect_branch` edge of its
    // own (its producer never emits one without a proven candidate set), so
    // `reached_indirect_targets` is always empty for it here and there is
    // no `indirect_target_set` to dereference.
    if ((is_indirect_call || is_indirect_branch) &&
        !tier1_pre_pc_stops.contains(terminal.source.address.value)) {
      std::vector<Address> candidate_values;
      candidate_values.reserve(indirect_target_set->candidates.size());
      for (const auto &candidate : indirect_target_set->candidates) candidate_values.push_back(candidate.value);
      std::sort(reached_indirect_targets.begin(), reached_indirect_targets.end());
      if (reached_indirect_targets != candidate_values || direct_count != 0U || fallthrough_count != 0U ||
          continuation_count != 0U || return_count != 0U || call_count != 0U)
        return "/* translation rejected: invalid C4 indirect edge set */\n";
    }
    const bool unconditional = terminal_operation->condition == M68kCondition::always;
    const bool runtime_routed_ownerless_return =
        is_return && return_count == 0U &&
        is_runtime_routed_ownerless_return(block.id.entry.value, emitted_instruction_sources,
                                           !runtime_return_target_set.empty());
    // SEG-007-T134 correction (ADR-0011 Decision §1): `is_return`'s own
    // completeness requirement no longer demands an exact edge count
    // matching "how many frames call THIS block's own entry" -- that closed
    // form was only valid for a callee whose entry block IS its own RTS
    // terminal. A retained RTS terminal simply requires at least one
    // validly-bound `return_to_continuation` edge (each individually proven
    // above); the actual runtime dispatch target is the literal popped
    // stack value validated against the whole-program continuation set
    // below, not a statically predicted count.
    if (((is_branch || is_direct_jump) &&
         (direct_count != 1U || fallthrough_count != ((unconditional || is_direct_jump) ? 0U : 1U) ||
                        edges_by_source[terminal.source.address.value].size() != direct_count + fallthrough_count)) ||
        (is_call && (call_count != 1U || direct_count != 0U || fallthrough_count != 0U || return_count != 0U)) ||
        (is_return && ((!runtime_routed_ownerless_return && return_count == 0U) ||
                       direct_count != 0U || fallthrough_count != 0U || call_count != 0U)) ||
        // SEG-007-T181 / ADR-0027: a fallthrough_continuation edge is valid only
        // on a non-terminal or call terminal, exactly one per terminal, and
        // never combined with a branch/return/fallthrough tail.
        (continuation_count != 0U &&
         (continuation_count != 1U || is_branch || is_direct_jump || is_return || is_indirect_branch || is_indirect_call ||
          direct_count != 0U || fallthrough_count != 0U || return_count != 0U)))
      return "/* translation rejected: incomplete C4 static edge */\n";
    ShardUnitScope block_unit(out, "block", block.id.entry.value, genesis_unit_declaration("genesis_block_", block.id.entry.value));
    out << "\nstatic GenesisControlTransfer genesis_block_" << std::uppercase << std::hex << std::setw(8)
        << std::setfill('0') << block.id.entry.value << "(GenesisRuntime *runtime) {\n";
    out << "  switch (runtime->pc) {\n";
    {
      const auto cut = c4_block_stops.find(block.id.entry.value);
      for (const auto &instruction : block.instructions) {
        const auto address = instruction.source.address.value;
        out << "  case UINT32_C(" << hex(address, 8) << "): goto genesis_instruction_"
            << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << address << ";\n";
        if (tier1_pre_pc_stops.contains(address) ||
            (cut != c4_block_stops.end() && cut->second.first == address))
          break;
      }
    }
    out << "  default: return genesis_internal_dispatch_inconsistency_stop(runtime);\n"
        << "  }\n";
    GenesisM68kEmissionContext memory{};
  memory.execution_history_hooks = g_execution_history_hooks;
    memory.program_counter = "runtime->pc";
    memory.address_registers = "runtime->a";
    memory.user_stack_pointer = "runtime->usp";
    bool block_cut = false;
    for (const auto &provenance : block.instructions) {
      out << "genesis_instruction_" << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
          << provenance.source.address.value << ":\n";
      const auto cut = c4_block_stops.find(block.id.entry.value);
      if (tier1_pre_pc_stops.contains(provenance.source.address.value)) {
        std::ostringstream suffix;
        suffix << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
               << provenance.source.address.value;
        out << "  return genesis_tier1_indirect_stop_" << suffix.str() << "(runtime);\n";
        block_cut = true;
        break;
      }
      if (cut != c4_block_stops.end() && provenance.source.address.value == cut->second.first) {
        std::ostringstream suffix;
        suffix << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << cut->second.first;
        out << "  return genesis_c4_lowering_stop_" << suffix.str() << "(runtime);\n";
        block_cut = true;
        break;
      }
      const auto found = operations.find(provenance.source.address.value);
      if (found == operations.end() || !same_provenance(found->second->provenance, provenance))
        return "/* translation rejected: invalid C4 prefix */\n";
      const auto retirement_cycles = m68k_retirement_cycle_expression(*found->second);
      if (!retirement_cycles)
        return "/* translation rejected: unaccounted MC68000 instruction timing */\n";
      const auto fact_for = [&](M68kStaticMemoryFactRole role) -> const M68kStaticMemoryFact * {
        const auto found = facts.find({provenance.source.address.value, role});
        return found == facts.end() ? nullptr : found->second;
      };
      // SEG-007-T252 / ADR-0040: the former SEG-007-T107 finite-loop-progress
      // proof lookup that fed only `memory.loop_progress_object` /
      // `memory.finite_loop_progress_proof` watchdog-note emission has been
      // removed; termination/progress policy now belongs to the runner.
      // Routed lowering creates per-operation C locals. Keep each operation
      // in its own compound statement so repeated EA forms in a static block
      // cannot redeclare those deterministic temporary names.
      out << "  {\n";
      if (found->second->kind == M68kIrKind::dbcc_loop)
        out << "    uint8_t m68k_dbcc_took_branch = 0U;\n";
      // Only the Dn form's retirement time depends on the condition (memory forms have a static row).
      if (found->second->kind == M68kIrKind::set_conditional &&
          found->second->destination_ea.mode == M68kEaMode::data_register)
        out << "    uint8_t m68k_scc_true = 0U;\n";
      if (found->second->kind == M68kIrKind::multiply_signed_word || found->second->kind == M68kIrKind::multiply_unsigned_word)
        out << "    uint16_t m68k_timing_mul_source = UINT16_C(0);\n";
      switch (found->second->kind) {
      case M68kIrKind::write_moveq:
      case M68kIrKind::subtract_quick_long_d0:
      // SEG-007-T165: plain `subtract_quick` (SUBQ) was previously grouped
      // into this same plain, unrouted `memory` group, but unlike
      // `subtract_quick_long_d0` (the tightly-scoped SUBQ.L #1,D0 register-
      // only special case, which genuinely never touches memory), SUBQ's
      // shared decode (libs/cpu/m68k/src/decode.cpp) accepts the full
      // `m68k_ea_data_alterable` destination set -- including every
      // register-indirect/predecrement/postincrement/indexed/displacement
      // memory mode. Emitting it through this unrouted context (empty
      // `ram_array`, `runtime_routing` unset) produced a malformed
      // `<cast>[<offset>]` read/write with no array identifier whenever its
      // destination was actually a memory operand, reaching a
      // `strict_c11_compile_failed` terminal only once static discovery
      // advanced far enough to expand through such a SUBQ. It now joins the
      // routed group below, alongside `add_quick`/`add`, which already
      // handles this exact same-shaped RMW-destination-with-immediate-
      // source case correctly.
      case M68kIrKind::load_effective_address:
      case M68kIrKind::general_branch:
      // SEG-007-T114: NOP has no operand, no EA, and no memory access at all,
      // so it joins this same plain, unrouted `memory` group; its only lowered
      // effect is the two-byte PC advance (see emit_m68k_operation_c's
      // M68kIrKind::no_operation case).
      case M68kIrKind::no_operation:
        out << emit_m68k_operation_c(*found->second, "runtime->d", "runtime->sr", "  ", &memory);
        break;
      // SEG-021-T018 / ADR 0043: the status-register / USP transfer family (MOVE to SR, MOVE to CCR, MOVE from
      // SR, ANDI/ORI/EORI to CCR/SR, MOVE USP both directions) is routed: the privileged forms raise vector 8
      // through the platform in user mode and stop through it when T would be set, and every legal operand mode
      // (supersedes the former Dn/#imm-only plain-group carve-out) reads/writes memory through the routed gate.
      // A non-auto-update foldable operand uses its retained fact exactly like CMP (MOVE to SR/CCR source) or
      // memory Scc (MOVE from SR destination, read then written); the lowering advances the configured
      // program counter directly (no macro bridge).
      case M68kIrKind::write_user_stack_pointer:
      case M68kIrKind::read_user_stack_pointer:
      case M68kIrKind::logical_immediate_to_ccr:
      case M68kIrKind::logical_immediate_to_sr:
      case M68kIrKind::write_status_register:
      case M68kIrKind::write_condition_codes:
      // SEG-021-T019 / ADR 0043: the software-exception family raises through the platform (routed), RTR pops its
      // frame through the platform's frame-return routine, and CHK.W's word bound has MOVE to SR's source shape.
      case M68kIrKind::trap_exception:
      case M68kIrKind::trap_on_overflow:
      case M68kIrKind::check_bounds:
      case M68kIrKind::return_restore_condition_codes:
      case M68kIrKind::instruction_exception:
      case M68kIrKind::read_status_register: {
        const bool reads_destination = found->second->kind == M68kIrKind::read_status_register;
        const auto *fact = fact_for(reads_destination ? M68kStaticMemoryFactRole::destination_read
                                                      : M68kStaticMemoryFactRole::source_read);
        const auto &operand = reads_destination ? found->second->destination_ea : found->second->source_ea;
        auto routed = memory;
        routed.runtime_routing = true;
        routed.runtime_object = "runtime";
        if (reads_destination && fact != nullptr) {
          const auto *write_fact = fact_for(M68kStaticMemoryFactRole::destination_write);
          if (write_fact == nullptr || write_fact->region != M68kAbsoluteOperandRegion::synthetic_work_ram ||
              fact->region != M68kAbsoluteOperandRegion::synthetic_work_ram)
            return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        }
        if (fact != nullptr) {
          routed.test_operand_access = genesis_lowering_access(fact->region);
          if (fact->region == M68kAbsoluteOperandRegion::raw_cartridge_rom)
            routed.test_operand_value = fact->immutable_value;
          if (fact->region != M68kAbsoluteOperandRegion::synthetic_work_ram &&
              fact->region != M68kAbsoluteOperandRegion::raw_cartridge_rom &&
              fact->region != M68kAbsoluteOperandRegion::controller_io &&
              fact->region != M68kAbsoluteOperandRegion::vdp &&
              fact->region != M68kAbsoluteOperandRegion::routed_device)
            return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        } else if (m68k_is_statically_foldable_control_ea(operand) &&
                   found->second->kind != M68kIrKind::logical_immediate_to_ccr &&
                   found->second->kind != M68kIrKind::logical_immediate_to_sr) {
          return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        }
        out << emit_m68k_operation_c(*found->second, "runtime->d", "runtime->sr", "  ", &routed);
        break;
      }
      // SEG-007-T177: EXT.W/EXT.L's decoded operand is fixed to
      // `M68kEaMode::data_register` by the shared ext_w/ext_l decode path
      // (decode.cpp always constructs `{M68kEaMode::data_register, reg, 0, 0,
      // 0, 0}`) -- never a mode that needs a resolver fact / runtime-routed
      // context, exactly like write_condition_codes above. Unlike that plain
      // group, though, its shared emission body (m68k.cpp, shared with
      // SWAP/NOT) advances the conventional literal `pc` spelling rather than
      // consulting `memory->program_counter`, so this case needs the same
      // `#define pc runtime->pc` / `#undef pc` wrapper `logical_not` (NOT)
      // above and shift_rotate_register/dbcc_loop below already use.
      // SEG-007-T224: SWAP shares EXT's exact Dn-only operand contract and
      // its lowerer also uses the conventional literal `pc` spelling. Keep it
      // in this non-routed macro-bridge group rather than the routed EA group:
      // SWAP has no memory operand to route or retain a fact for.
      case M68kIrKind::write_swap:
      case M68kIrKind::sign_extend_word:
      case M68kIrKind::sign_extend_long:
        out << "#define pc runtime->pc\n"
            << emit_m68k_operation_c(*found->second, "runtime->d", "runtime->sr", "  ", &memory)
            << "#undef pc\n";
        break;
      // SEG-007-T152: the register-count shift/rotate form's destination_ea
      // is architecturally always `data_register` (see
      // m68k_c4_represented_ir_kind's own comment) and its source_ea is
      // restricted to `immediate` or `data_register` -- never a mode that
      // needs a resolver fact / runtime-routed context -- so no fact lookup
      // or runtime routing is needed here, exactly like write_condition_codes
      // above. Unlike that plain group, though, its own emit_c_update owner
      // (shared with the direct_flow C6/C7 shift/rotate lowering) emits the
      // conventional literal `pc` spelling rather than consulting
      // `memory->program_counter`, so this case needs the same `#define pc
      // runtime->pc` / `#undef pc` wrapper as dbcc_loop below.
      case M68kIrKind::shift_rotate_register:
        out << "#define pc runtime->pc\n"
            << emit_m68k_operation_c(*found->second, "runtime->d", "runtime->sr", "  ", &memory)
            << "#undef pc\n";
        break;
      case M68kIrKind::dbcc_loop: {
        // The established DBcc lowerer owns condition/decrement/target
        // semantics but uses its conventional local `pc` spelling.
        // SEG-007-T252 / ADR-0040: this block's former SEG-007-T107
        // loop-progress watchdog note emission has been removed; the runner
        // (`genesis_runtime_run`'s dispatch allowance) now owns
        // termination/progress policy. The caller-owned timing byte remains
        // local to this retiring block.
        auto dbcc_memory = memory;
        dbcc_memory.timing_dbcc_taken = "m68k_dbcc_took_branch";
        out << "#define pc runtime->pc\n"
            << emit_m68k_operation_c(*found->second, "runtime->d", "runtime->sr", "  ", &dbcc_memory)
            << "#undef pc\n";
        break;
      }
      // SEG-007-T153: ADDQ (`add_quick`) shares the `add` family's emission
      // body verbatim, including the routed read-modify-write destination path
      // and the add-family deferred single-address-register commit path for an
      // auto-updating destination. It needs the identical routed memory
      // context `add` builds below (never the plain unrouted `memory`): its
      // immediate quick source never carries a fact, but a foldable absolute
      // work_ram destination still needs its retained destination_read /
      // destination_write facts exactly like `add`.
      case M68kIrKind::add_quick:
      case M68kIrKind::add_immediate:
      // SEG-007-T167: AND/OR/EOR (`<ea>,Dn` or `Dn,<ea>`) reuse the add
      // family's source-read / destination read-modify-write fact-threading
      // shape verbatim. Exactly one operand is a data register; the other, if
      // a foldable-control memory EA, is routed through the same
      // synthetic_work_ram-only retained fact the add family requires. An
      // auto-updating operand never reaches here -- classify_m68k_c4_gap_shapes
      // declines it as requires_architecture_decision and the C4 emitter
      // rejects the whole prefix first (no deferred-address-commit contract
      // for this family). The shared logical emitter advances `pc` through a
      // literal identifier, so the same `#define pc` bridge the add case uses
      // applies unchanged.
      case M68kIrKind::logical_and:
      case M68kIrKind::logical_or:
      case M68kIrKind::exclusive_or:
      // SEG-007-T170: SUB (`<ea>,Dn` or `Dn,<ea>`) shares ADD's exact
      // source-read / destination read-modify-write fact-threading shape
      // verbatim -- exactly one operand is a data register, the other, if a
      // foldable-control memory EA, is routed through the same
      // synthetic_work_ram-only retained fact ADD requires. An
      // auto-updating operand never reaches here -- classify_m68k_c4_gap_
      // shapes declines it as requires_architecture_decision and the C4
      // emitter rejects the whole prefix first (this family has no
      // deferred-address-commit contract, matching CMP/SUBA/the logical
      // family above). SEG-007-T174 correction: SUBTRACT's plain (non-auto-
      // update) destination-write path is shared with SUBQ/SUBI's own
      // sibling case below, which needs the fully-qualified
      // `memory->program_counter` text (no bridge there); THIS caller's
      // `#define pc` bridge is required by the sibling ADD/logical bodies,
      // which always emit the bare `pc` spelling, so `routed
      // .pc_macro_bridge_active` below tells the shared m68k.cpp emitter
      // which of the two textual conventions this specific call needs
      // (previously assumed "harmless" for SUBTRACT here, which a
      // Ghidra-assisted real-ROM candidate route proved false: emitting the
      // fully-qualified text inside this bridge's scope re-exposes its own
      // trailing `pc` token to the macro a second time).
      case M68kIrKind::subtract:
      case M68kIrKind::add: {
        const auto *source = fact_for(M68kStaticMemoryFactRole::source_read);
        const auto *destination_read = fact_for(M68kStaticMemoryFactRole::destination_read);
        const auto *destination_write = fact_for(M68kStaticMemoryFactRole::destination_write);
        if ((destination_read == nullptr) != (destination_write == nullptr) ||
            (destination_read != nullptr && destination_read->region != M68kAbsoluteOperandRegion::synthetic_work_ram) ||
            (destination_write != nullptr && destination_write->region != M68kAbsoluteOperandRegion::synthetic_work_ram) ||
            (source != nullptr && destination_read != nullptr))
          return "/* translation rejected: unsupported C4 operation */\n";
        auto routed = memory;
        routed.runtime_routing = true;
        routed.runtime_object = "runtime";
        routed.pc_macro_bridge_active = true;
        const auto apply_read_fact = [&](const M68kStaticMemoryFact *fact) {
          if (fact == nullptr) return true;
          routed.test_operand_access = genesis_lowering_access(fact->region);
          if (fact->region == M68kAbsoluteOperandRegion::raw_cartridge_rom)
            routed.test_operand_value = fact->immutable_value;
          return fact->region == M68kAbsoluteOperandRegion::synthetic_work_ram ||
                 fact->region == M68kAbsoluteOperandRegion::raw_cartridge_rom ||
                 fact->region == M68kAbsoluteOperandRegion::controller_io ||
                 fact->region == M68kAbsoluteOperandRegion::vdp ||
                 fact->region == M68kAbsoluteOperandRegion::routed_device;
        };
        if (!apply_read_fact(source != nullptr ? source : destination_read))
          return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        out << "#define pc runtime->pc\n"
            << emit_m68k_operation_c(*found->second, "runtime->d", "runtime->sr", "  ", &routed)
            << "#undef pc\n";
        break;
      }
      case M68kIrKind::add_address:
      case M68kIrKind::subtract_address:
      case M68kIrKind::compare:
      case M68kIrKind::compare_address:
      // SEG-007-T220: MULS.W <ea>,Dn shares this exact same source-read
      // fact/routing shape -- its destination is architecturally always Dn
      // (never a mode needing a fact), and its shared m68k.cpp emission body
      // also always emits the bare `pc` spelling unconditionally, exactly
      // like the compare-family bodies this case block already documents as
      // no-ops for the `pc_macro_bridge_active` flag above.
      case M68kIrKind::multiply_signed_word:
      case M68kIrKind::multiply_unsigned_word:
      case M68kIrKind::divide_signed_word:
      case M68kIrKind::divide_unsigned_word: {
        // SEG-007-T222: MULU.W/DIVS.W/DIVU.W share this exact same source-
        // read fact/routing shape as MULS.W above -- their destination is
        // likewise architecturally always Dn (never a mode needing a fact),
        // and their shared m68k.cpp emission bodies also always emit the
        // bare `pc` spelling unconditionally (inside this same
        // `#define pc runtime->pc` bridge).
        //
        // SEG-007-T146: plain `CMP <ea>,Dn` reuses SUBA/CMPA's source-read
        // fact/routing shape verbatim -- the shared emitter computes
        // `Dn - source` at width and updates CCR only, never writing a result.
        //
        // SEG-007-T174 follow-up fix: `subtract_address` (SUBA) shares
        // m68k.cpp's subtract-family emission body with plain `subtract`/
        // `subtract_immediate`/`subtract_quick`, and that shared body's
        // trailing PC-advance textually depends on
        // `memory->pc_macro_bridge_active` -- when true it emits the bare
        // `pc` spelling (safe inside this call's own
        // `#define pc runtime->pc` bridge below); when false (the default)
        // it emits the fully-qualified `runtime->pc` text, whose own
        // trailing `pc` token gets re-expanded by this call's still-active
        // macro into `runtime->runtime->pc`, breaking strict-C11
        // compilation. This caller previously left the flag at its default
        // `false`, so it must be forced `true` here. `add_address` (ADDA)
        // uses a *different* shared body (the add family) that always emits
        // the bare `pc` spelling unconditionally, and `compare`/
        // `compare_address` use yet another shared body that also always
        // emits the bare `pc` spelling unconditionally -- neither reads this
        // flag at all, so forcing it `true` for the whole shared case block
        // is provably a no-op for those three kinds and the required fix for
        // `subtract_address`.
        const auto *source = fact_for(M68kStaticMemoryFactRole::source_read);
        if (source == nullptr && m68k_is_statically_foldable_control_ea(found->second->source_ea))
          return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        auto routed = memory;
        routed.runtime_routing = true;
        routed.runtime_object = "runtime";
        routed.pc_macro_bridge_active = true;
        if (found->second->kind == M68kIrKind::multiply_signed_word ||
            found->second->kind == M68kIrKind::multiply_unsigned_word)
          routed.timing_mul_source = "m68k_timing_mul_source";
        if (source != nullptr) {
          routed.test_operand_access = genesis_lowering_access(source->region);
          if (source->region == M68kAbsoluteOperandRegion::raw_cartridge_rom)
            routed.test_operand_value = source->immutable_value;
          if (source->region != M68kAbsoluteOperandRegion::synthetic_work_ram &&
              source->region != M68kAbsoluteOperandRegion::raw_cartridge_rom &&
              source->region != M68kAbsoluteOperandRegion::controller_io &&
              source->region != M68kAbsoluteOperandRegion::vdp &&
              source->region != M68kAbsoluteOperandRegion::routed_device)
            return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        }
        out << "#define pc runtime->pc\n"
            << emit_m68k_operation_c(*found->second, "runtime->d", "runtime->sr", "  ", &routed)
            << "#undef pc\n";
        break;
      }
      case M68kIrKind::compare_immediate: {
        // SEG-007-T146: `CMPI #imm,<ea>` -- immediate source, destination is
        // read (never written) for the CCR-only result. Same destination-read
        // fact/routing shape as BTST.
        const auto *destination = fact_for(M68kStaticMemoryFactRole::destination_read);
        auto routed = memory;
        routed.runtime_routing = true;
        routed.runtime_object = "runtime";
        if (destination != nullptr) {
          routed.test_operand_access = genesis_lowering_access(destination->region);
          if (destination->region == M68kAbsoluteOperandRegion::raw_cartridge_rom)
            routed.test_operand_value = destination->immutable_value;
          if (destination->region != M68kAbsoluteOperandRegion::synthetic_work_ram &&
              destination->region != M68kAbsoluteOperandRegion::raw_cartridge_rom &&
              destination->region != M68kAbsoluteOperandRegion::controller_io &&
              destination->region != M68kAbsoluteOperandRegion::vdp &&
              destination->region != M68kAbsoluteOperandRegion::routed_device)
            return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        } else if (m68k_is_statically_foldable_control_ea(found->second->destination_ea)) {
          return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        }
        out << "#define pc runtime->pc\n"
            << emit_m68k_operation_c(*found->second, "runtime->d", "runtime->sr", "  ", &routed)
            << "#undef pc\n";
        break;
      }
      case M68kIrKind::bit_test: {
        const auto *destination = fact_for(M68kStaticMemoryFactRole::destination_read);
        auto routed = memory;
        routed.runtime_routing = true;
        routed.runtime_object = "runtime";
        if (destination != nullptr) {
          routed.test_operand_access = genesis_lowering_access(destination->region);
          if (destination->region == M68kAbsoluteOperandRegion::raw_cartridge_rom)
            routed.test_operand_value = destination->immutable_value;
          if (destination->region != M68kAbsoluteOperandRegion::synthetic_work_ram &&
              destination->region != M68kAbsoluteOperandRegion::raw_cartridge_rom &&
              destination->region != M68kAbsoluteOperandRegion::controller_io &&
              destination->region != M68kAbsoluteOperandRegion::vdp &&
              destination->region != M68kAbsoluteOperandRegion::routed_device)
            return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        }
        out << "#define pc runtime->pc\n"
            << emit_m68k_operation_c(*found->second, "runtime->d", "runtime->sr", "  ", &routed)
            << "#undef pc\n";
        break;
      }
      // SEG-007-T209: BCHG/BCLR/BSET share the exact bit-manipulation
      // emission body in m68k.cpp with the BTST case immediately above
      // (`case M68kIrKind::bit_test: ... bit_set:`), which advances `pc`
      // through the same bare literal spelling BTST uses, so this case needs
      // the identical `#define pc` bridge. Unlike BTST (destination_read
      // only), these are full read-modify-write operations on their
      // destination, so the resolver fact is checked against
      // destination_write -- the same "read side threaded from the write
      // fact" RMW shape write_clr/subtract_quick below already establish for
      // this switch, not BTST's own read-only shape.
      case M68kIrKind::bit_change:
      case M68kIrKind::bit_clear:
      case M68kIrKind::bit_set: {
        const auto *destination = fact_for(M68kStaticMemoryFactRole::destination_write);
        if (destination != nullptr && destination->region != M68kAbsoluteOperandRegion::synthetic_work_ram)
          return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        if (destination == nullptr && m68k_is_statically_foldable_control_ea(found->second->destination_ea))
          return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        auto routed = memory;
        routed.runtime_routing = true;
        routed.runtime_object = "runtime";
        if (destination != nullptr) routed.test_operand_access = genesis_lowering_access(destination->region);
        out << "#define pc runtime->pc\n"
            << emit_m68k_operation_c(*found->second, "runtime->d", "runtime->sr", "  ", &routed)
            << "#undef pc\n";
        break;
      }
      case M68kIrKind::write_clr: {
        const auto *destination = fact_for(M68kStaticMemoryFactRole::destination_write);
        if (destination != nullptr && destination->region != M68kAbsoluteOperandRegion::synthetic_work_ram)
          return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        if (destination == nullptr && m68k_is_statically_foldable_control_ea(found->second->destination_ea))
          return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        auto routed = memory;
        routed.runtime_routing = true;
        routed.runtime_object = "runtime";
        out << emit_m68k_operation_c(*found->second, "runtime->d", "runtime->sr", "  ", &routed);
        break;
      }
      // SEG-021-T011: PEA computes its `source_ea`'s address only (never
      // reading its contents, like LEA above), so -- unlike write_clr/bit_
      // change/etc. immediately above -- it never has a foldable-absolute
      // EA that could need a retained resolver fact (classify_m68k_c4_gap_
      // shapes emits no check_fact row for push_effective_address, the same
      // "represented, no further per-kind gap handling" treatment as LEA).
      // Its push target is architecturally fixed to -(A7), never a decoded
      // destination_ea, so no fact lookup applies to it either. It still
      // needs the routed context (unlike LEA, which never touches memory)
      // because its -(A7) push is a genuine RAM write, lowered through the
      // atomic local-snapshot/deferred-commit technique in
      // emit_m68k_operation_c's push_effective_address case.
      case M68kIrKind::push_effective_address: {
        auto routed = memory;
        routed.runtime_routing = true;
        routed.runtime_object = "runtime";
        out << emit_m68k_operation_c(*found->second, "runtime->d", "runtime->sr", "  ", &routed);
        break;
      }
      // SEG-007-T170: SUBI's source is always the instruction-embedded
      // immediate (never a memory fact); its destination is a full RMW
      // operand, sharing SUBQ's exact destination-only resolver-fact
      // pattern immediately below verbatim (same shared subtract-family
      // emitter, same "read side threaded from the write fact" shape).
      case M68kIrKind::subtract_immediate:
      case M68kIrKind::subtract_quick: {
        // SEG-007-T165: SUBQ's source is always the quick immediate (never a
        // memory fact); its destination is a full RMW operand, exactly like
        // write_clr's own destination-only resolver-fact pattern, except the
        // shared emitter (m68k.cpp) also READS the destination to form the
        // subtraction result -- the same "read side threaded from the write
        // fact" shape logical_and_immediate below already establishes for
        // ANDI. A register-indirect/predecrement/postincrement/indexed/
        // displacement destination needs no fact at all (retain_fact never
        // records one for those modes); only a foldable absolute destination
        // is rejected here when unsupported. Unlike ANDI/CLR (which advance
        // `pc` through a literal identifier), the shared subtract-family
        // emitter advances `memory->program_counter` directly for this
        // caller (`routed.pc_macro_bridge_active` stays at its default
        // `false`), so no `#define pc`/`#undef pc` bridge is needed here --
        // contrast the sibling `subtract`/`add` case above, which shares
        // this exact m68k.cpp emission body but sets that flag `true`
        // because its own bridge requires the bare `pc` spelling instead
        // (SEG-007-T174).
        const auto *destination = fact_for(M68kStaticMemoryFactRole::destination_write);
        if (destination != nullptr && destination->region != M68kAbsoluteOperandRegion::synthetic_work_ram)
          return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        if (destination == nullptr && m68k_is_statically_foldable_control_ea(found->second->destination_ea))
          return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        auto routed = memory;
        routed.runtime_routing = true;
        routed.runtime_object = "runtime";
        if (destination != nullptr) routed.test_operand_access = genesis_lowering_access(destination->region);
        out << emit_m68k_operation_c(*found->second, "runtime->d", "runtime->sr", "  ", &routed);
        break;
      }
      case M68kIrKind::shift_rotate_memory:
      case M68kIrKind::logical_not: {
        // SEG-007-T168: NOT has no second operand at all (unlike SUBQ's
        // quick-immediate source); its sole destination is a full RMW
        // operand, mirroring subtract_quick's own fact-lookup/region-
        // threading pattern immediately above verbatim.
        const auto *destination = fact_for(M68kStaticMemoryFactRole::destination_write);
        if (destination != nullptr && destination->region != M68kAbsoluteOperandRegion::synthetic_work_ram)
          return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        if (destination == nullptr && m68k_is_statically_foldable_control_ea(found->second->destination_ea))
          return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        auto routed = memory;
        routed.runtime_routing = true;
        routed.runtime_object = "runtime";
        if (destination != nullptr) routed.test_operand_access = genesis_lowering_access(destination->region);
        out << "#define pc runtime->pc\n"
            << emit_m68k_operation_c(*found->second, "runtime->d", "runtime->sr", "  ", &routed)
            << "#undef pc\n";
        break;
      }
      case M68kIrKind::negate_decimal:
      // SEG-021-T016: TAS and memory Scc are byte one-address read-then-write operands; both use this fact-lookup /
      // region-threading discipline and advance the configured program counter directly (no macro bridge).
      case M68kIrKind::test_and_set:
      case M68kIrKind::set_conditional:
      case M68kIrKind::negate_extended:
      case M68kIrKind::negate_word: {
        // SEG-021-T014: NEG/NEGX are one-address RMW operands with NOT's fact-lookup/region-threading
        // discipline (a foldable absolute destination needs a retained synthetic-work-RAM fact; every
        // other data-alterable mode, including auto-updating and indexed ones, is routed at runtime).
        // Their shared lowering advances the configured program-counter expression directly, so unlike
        // NOT's legacy bare-`pc` body it must not pass through the macro bridge, which would expand
        // `runtime->pc` into `runtime->runtime->pc` under strict C11.
        const auto *destination = fact_for(M68kStaticMemoryFactRole::destination_write);
        if (destination != nullptr && destination->region != M68kAbsoluteOperandRegion::synthetic_work_ram)
          return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        if (destination == nullptr && m68k_is_statically_foldable_control_ea(found->second->destination_ea))
          return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        auto routed = memory;
        routed.runtime_routing = true;
        routed.runtime_object = "runtime";
        if (destination != nullptr) routed.test_operand_access = genesis_lowering_access(destination->region);
        if (found->second->kind == M68kIrKind::set_conditional && found->second->destination_ea.mode == M68kEaMode::data_register)
          routed.timing_scc_true = "m68k_scc_true";  // SEG-021-T016: only Scc Dn (condition-dependent timing) writes it
        out << emit_m68k_operation_c(*found->second, "runtime->d", "runtime->sr", "  ", &routed);
        break;
      }
      // SEG-021-T014: ADDX/SUBX (`Dy,Dx` and `-(Ay),-(Ax)`) and CMPM (`(Ay)+,(Ax)+`) never carry an absolute
      // or PC-relative operand, so no retained fact exists or is needed; the memory pairs are lowered by the
      // operation-local deferred address commit and advance `memory->program_counter` directly (no bridge).
      case M68kIrKind::add_decimal:
      case M68kIrKind::subtract_decimal:
      // SEG-021-T016: EXG (register-only) and MOVEP (d16(An), never a retained fact) advance `memory->program_counter`.
      case M68kIrKind::exchange_registers:
      case M68kIrKind::movep_transfer:
      case M68kIrKind::add_extended:
      case M68kIrKind::subtract_extended:
      case M68kIrKind::compare_memory: {
        auto routed = memory;
        routed.runtime_routing = true;
        routed.runtime_object = "runtime";
        out << emit_m68k_operation_c(*found->second, "runtime->d", "runtime->sr", "  ", &routed);
        break;
      }
      // SEG-007-T167: ORI / EORI (`#imm,<ea>`) share ANDI's exact shape --
      // instruction-embedded immediate source, a read-modify-write
      // destination whose retained destination_write fact must resolve to
      // synthetic work RAM, and the same shared logical emitter body. An
      // auto-updating destination is declined upstream by
      // classify_m68k_c4_gap_shapes, exactly as it is for ANDI.
      case M68kIrKind::logical_or_immediate:
      case M68kIrKind::exclusive_or_immediate:
      case M68kIrKind::logical_and_immediate: {
        // SEG-007-T071: ANDI's destination fact-lookup/rejection shape
        // reuses write_clr's own destination-only resolver-fact pattern
        // verbatim (its source is always immediate, never a memory fact) --
        // the same "fits directly" precedent write_clr already established,
        // confirmed by direct source inspection of retain_fact/require_fact
        // above (this task extended both to cover `andi` identically to
        // `clr`). Unlike write_clr (a destination-only write), ANDI's shared
        // emitter (emit_m68k_operation_c) also READS the destination via
        // m68k_emit_ea_read to form the AND result, and that read's own
        // absolute_word/absolute_long branch requires
        // memory->test_operand_region to be set (the same field test_operand/
        // write_move already populate for their own SOURCE read); a
        // destination fact, once its region is confirmed synthetic_work_ram
        // above, is threaded into that field so the destination's read side
        // is representable too, not only its write side.
        const auto *destination = fact_for(M68kStaticMemoryFactRole::destination_write);
        if (destination != nullptr && destination->region != M68kAbsoluteOperandRegion::synthetic_work_ram)
          return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        if (destination == nullptr && m68k_is_statically_foldable_control_ea(found->second->destination_ea))
          return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        auto routed = memory;
        routed.runtime_routing = true;
        routed.runtime_object = "runtime";
        if (destination != nullptr) routed.test_operand_access = genesis_lowering_access(destination->region);
        // Unlike write_clr's own per-instruction emitter (which already
        // writes through the shared `memory->program_counter` field),
        // logical_and_immediate's shared emitter (shared with logical_and/
        // logical_or/logical_or_immediate/exclusive_or/exclusive_or_
        // immediate) advances PC through a literal `pc` identifier, exactly
        // like write_move/movem_transfer above; the same `#define`/`#undef`
        // bridge binds it to `runtime->pc` for this block-local emission
        // only.
        out << "#define pc runtime->pc\n"
            << emit_m68k_operation_c(*found->second, "runtime->d", "runtime->sr", "  ", &routed)
            << "#undef pc\n";
        break;
      }
      case M68kIrKind::write_move: {
        // SEG-007-T068: generalized onto the same resolver-fact/runtime-
        // routing pattern write_clr (destination) and test_operand (source)
        // already use, in place of the prior audited_ram_move_addresses-
        // only long-word Dn<->RAM-absolute gate.
        const auto *destination = fact_for(M68kStaticMemoryFactRole::destination_write);
        // SEG-007-T113 / SEG-007-T115: a foldable absolute destination is
        // representable when its retained fact resolves to synthetic work RAM
        // (a RAM-array or routed store), to the VDP register window (a routed
        // genesis_route_access WORD/LONG WRITE), or to a generalized
        // routed_device window (Z80 bus arbitration / Z80 program RAM / PSG
        // port -- also a routed genesis_route_access store). In every routed
        // case the runtime device gate is the sole semantic owner. Every
        // other retained destination region stays unrepresentable here.
        if (destination != nullptr &&
            destination->region != M68kAbsoluteOperandRegion::synthetic_work_ram &&
            destination->region != M68kAbsoluteOperandRegion::vdp &&
            destination->region != M68kAbsoluteOperandRegion::routed_device)
          return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        if (destination == nullptr && m68k_is_statically_foldable_control_ea(found->second->destination_ea))
          return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        const auto *source = fact_for(M68kStaticMemoryFactRole::source_read);
        auto routed = memory;
        // A write-side fact absence for a non-foldable EA is legitimate
        // (register-direct/register-indirect never records one) and always
        // needs routing, exactly like write_clr's own unconditional
        // routing; this also always routes the destination side, never a
        // private RAM-array index, regardless of the source branch below.
        routed.runtime_routing = true;
        routed.runtime_object = "runtime";
        if (source == nullptr) {
          if (m68k_is_statically_foldable_control_ea(found->second->source_ea))
            return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        } else if (source->region == M68kAbsoluteOperandRegion::synthetic_work_ram) {
          routed.test_operand_access = genesis_lowering_access(source->region);
        } else if (source->region == M68kAbsoluteOperandRegion::raw_cartridge_rom) {
          routed.test_operand_access = genesis_lowering_access(source->region);
          routed.test_operand_value = source->immutable_value;
        } else if (source->region == M68kAbsoluteOperandRegion::controller_io ||
                   source->region == M68kAbsoluteOperandRegion::vdp ||
                   source->region == M68kAbsoluteOperandRegion::routed_device) {
          routed.test_operand_access = genesis_lowering_access(source->region);
        } else {
          return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        }
        // SEG-007-T070: write_move's own block-emission case (in
        // emit_m68k_operation_c) now independently declines one narrow
        // operand combination -- its own same-register aliasing rejection
        // (docs/architecture/c4-move-predecrement-postincrement-commit-
        // contract.md, Q3) -- by emitting no C for the instruction at all.
        // Discovery's coarse per-kind block/frontier classification has no
        // visibility into that one narrow, decode-independent detail, so an
        // instruction it declines is not necessarily its own block's sole
        // frontier; without this check, such an instruction's own silently
        // empty generated C would leave a hole mid-block with no advancing
        // `pc +=`, rather than a clean rejection. This generically detects
        // *any* reason write_move's own case might emit nothing (not only
        // Q3), the same "declined" signal every other case in this switch
        // already surfaces via its own explicit pre-check.
        // SEG-007-T252 / ADR-0040: the former SEG-007-T155/ADR-0017 (WRITE)
        // and SEG-007-T157/ADR-0019 (READ) data-transform progress proof
        // lookup that fed only the now-removed guarded watchdog note has been
        // removed; termination/progress policy belongs to the runner.
        const auto emitted = emit_m68k_operation_c(*found->second, "runtime->d", "runtime->sr", "  ", &routed);
        // emit_m68k_operation_c always writes its `indent` argument first,
        // even when its own switch case emits nothing further, so an
        // exact-indent-only result is this function's "declined, no C for
        // this instruction" signal (mirrored from write_move's own
        // `read.ok`/`write.ok` checks and, now, Q3).
        if (emitted == "  ") return "/* translation rejected: C4 write_move operand combination is not representable */\n";
        out << "#define pc runtime->pc\n" << emitted << "#undef pc\n";
        break;
      }
      case M68kIrKind::write_movea: {
        const auto *source = fact_for(M68kStaticMemoryFactRole::source_read);
        if (source == nullptr && m68k_is_statically_foldable_control_ea(found->second->source_ea))
          return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        auto routed = memory;
        if (source == nullptr) {
          routed.runtime_routing = true;
          routed.runtime_object = "runtime";
        } else if (source->region == M68kAbsoluteOperandRegion::synthetic_work_ram) {
          routed.test_operand_access = genesis_lowering_access(source->region);
          routed.runtime_routing = true;
          routed.runtime_object = "runtime";
        } else if (source->region == M68kAbsoluteOperandRegion::raw_cartridge_rom) {
          routed.test_operand_access = genesis_lowering_access(source->region);
          routed.test_operand_value = source->immutable_value;
        } else if (source->region == M68kAbsoluteOperandRegion::controller_io ||
                   source->region == M68kAbsoluteOperandRegion::vdp ||
                   source->region == M68kAbsoluteOperandRegion::routed_device) {
          routed.test_operand_access = genesis_lowering_access(source->region);
          routed.runtime_routing = true;
          routed.runtime_object = "runtime";
        } else {
          return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        }
        // The preflight rejects auto-updating sources before this point.  This
        // generic path therefore has no uncommitted An mutation to recover on
        // a routed read failure.
        out << emit_m68k_operation_c(*found->second, "runtime->d", "runtime->sr", "  ", &routed);
        break;
      }
      case M68kIrKind::test_operand: {
        const auto *source = fact_for(M68kStaticMemoryFactRole::source_read);
        auto routed = memory;
        if (source == nullptr) {
          if (m68k_is_statically_foldable_control_ea(found->second->source_ea))
            return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
          routed.runtime_routing = true;
          routed.runtime_object = "runtime";
        } else if (source->region == M68kAbsoluteOperandRegion::synthetic_work_ram) {
          routed.test_operand_access = genesis_lowering_access(source->region);
          routed.runtime_routing = true;
          routed.runtime_object = "runtime";
        } else if (source->region == M68kAbsoluteOperandRegion::raw_cartridge_rom) {
          routed.test_operand_access = genesis_lowering_access(source->region);
          routed.test_operand_value = source->immutable_value;
        } else if (source->region == M68kAbsoluteOperandRegion::controller_io ||
                   source->region == M68kAbsoluteOperandRegion::vdp ||
                   source->region == M68kAbsoluteOperandRegion::routed_device) {
          routed.test_operand_access = genesis_lowering_access(source->region);
          routed.runtime_routing = true;
          routed.runtime_object = "runtime";
        } else {
          return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
        }
        out << emit_m68k_operation_c(*found->second, "runtime->d", "runtime->sr", "  ", &routed);
        break;
      }
      case M68kIrKind::call_general:
      case M68kIrKind::bsr_call: {
        if (is_indirect_call && found->second->kind == M68kIrKind::call_general &&
            same_provenance(provenance, terminal_decoded->provenance)) {
          // SEG-007-T124 / ADR-0009: the multi-candidate indirect JSR
          // terminal. Its complete shape (exactly the retained
          // `indirect_call` edges/frames matching `indirect_target_set`) was
          // already independently validated above; this lowers only the
          // proven candidate array and the runtime membership guard. (Tier 2,
          // ADR-0024, never reaches this per-block path at all -- see its own
          // dedicated lowering in `build_genesis_frontier_stop_function`.)
          if (indirect_target_set == nullptr) return "/* translation rejected: invalid C4 indirect target set */\n";
          auto routed = memory;
          routed.runtime_routing = true;
          routed.runtime_object = "runtime";
          routed.continuation = static_cast<Address>(terminal.source.address.value + terminal.length.value);
          std::vector<std::uint32_t> candidate_values;
          candidate_values.reserve(indirect_target_set->candidates.size());
          for (const auto &candidate : indirect_target_set->candidates) candidate_values.push_back(candidate.value);
          routed.indirect_candidate_targets = candidate_values;
          out << emit_m68k_operation_c(*found->second, "runtime->d", "runtime->sr", "  ", &routed);
          break;
        }
        const auto call = std::find_if(edges_by_source[provenance.source.address.value].begin(),
                                       edges_by_source[provenance.source.address.value].end(),
                                       [](const M68kStaticEdge *edge) { return edge->kind == M68kStaticEdgeKind::direct_call; });
        if (call == edges_by_source[provenance.source.address.value].end() || !(*call)->call)
          return "/* translation rejected: invalid C4 static call edge */\n";
        const auto effect = m68k_operation_effect(*found->second);
        if (effect.stack != M68kStackEffectKind::push_static_continuation || effect.stack_width != 4U ||
            effect.pc != M68kPcEffectKind::direct_target || effect.direct_target != (*call)->target.value)
          return "/* translation rejected: invalid C4 static call effect */\n";
        auto routed = memory;
        routed.runtime_routing = true;
        routed.runtime_object = "runtime";
        routed.continuation = (*call)->call->continuation.value;
        out << emit_m68k_operation_c(*found->second, "runtime->d", "runtime->sr", "  ", &routed);
        break;
      }
      case M68kIrKind::jump_general: {
        // SEG-007-T185: a direct (statically-folded absolute.w / absolute.l /
        // d16(PC)) JMP terminal -- lowered exactly like an unconditional
        // general_branch. `emit_m68k_operation_c` already assigns
        // `runtime->pc` from the folded `effect.direct_target` for this shape
        // (m68k.cpp jump_general/direct-target branch); it advances a literal
        // `pc`, so the same `#define pc runtime->pc` bridge the sign_extend /
        // shift_rotate_register / dbcc_loop cases already use applies here. The
        // direct_branch edge and the terminal's completeness were validated
        // above; no runtime routing, no candidate set.
        if (!is_indirect_branch) {
          if (!same_provenance(provenance, terminal_decoded->provenance))
            return "/* translation rejected: invalid C4 static edge */\n";
          const auto direct_jump_effect = m68k_operation_effect(*found->second);
          if (direct_jump_effect.pc != M68kPcEffectKind::direct_target)
            return "/* translation rejected: unsupported C4 operation */\n";
          out << "#define pc runtime->pc\n"
              << emit_m68k_operation_c(*found->second, "runtime->d", "runtime->sr", "  ", &memory)
              << "#undef pc\n";
          break;
        }
        if (!same_provenance(provenance, terminal_decoded->provenance) ||
            indirect_target_set == nullptr)
          return "/* translation rejected: invalid C4 indirect target set */\n";
        auto routed = memory;
        routed.runtime_routing = true;
        routed.runtime_object = "runtime";
        std::vector<std::uint32_t> candidate_values;
        candidate_values.reserve(indirect_target_set->candidates.size());
        for (const auto &candidate : indirect_target_set->candidates) candidate_values.push_back(candidate.value);
        routed.indirect_candidate_targets = candidate_values;
        out << emit_m68k_operation_c(*found->second, "runtime->d", "runtime->sr", "  ", &routed);
        break;
      }
      case M68kIrKind::movem_transfer: {
        // SEG-007-T067: MOVEM.W/L register-list transfer. Unlike
        // write_clr/test_operand, retain_fact never records a static memory
        // fact for MOVEM at all (see its own switch above -- move/tst/clr
        // only), so this case never consults `fact_for`; every legal MOVEM
        // EA family's per-slot address is instead a pure function of the
        // instruction's own statically-decoded EA (recomputed by
        // emit_m68k_operation_c's own shared movem_transfer routed branch),
        // routed through the exact same genesis_route_access boundary as
        // every other selected memory-affecting C4 kind. `m68k_ea_movem_
        // register_to_memory`/`m68k_ea_movem_memory_to_register` (decode's
        // own legal-EA sets) admit exactly the six families that routed
        // branch represents (absolute.w/absolute.l/d16(PC),
        // (An)/d16(An), -(An), (An)+, plus SEG-021-T012's
        // (d8,An,Xn)/(d8,PC,Xn) widening -- `emit_m68k_operation_c`'s own
        // shared movem_transfer routed branch groups these two new modes
        // with (An)/d16(An), the same one-time working-EA-snapshot
        // discipline); a forged/malformed IR naming any other EA mode is
        // rejected here, before any C is emitted, rather than silently
        // producing an incomplete block.
        const bool store = found->second->movem_direction == M68kMovemDirection::registers_to_memory;
        const auto &movem_ea = store ? found->second->destination_ea : found->second->source_ea;
        const bool representable =
            movem_ea.mode == M68kEaMode::absolute_word || movem_ea.mode == M68kEaMode::absolute_long ||
            movem_ea.mode == M68kEaMode::pc_disp16 || movem_ea.mode == M68kEaMode::address_indirect ||
            movem_ea.mode == M68kEaMode::address_disp16 || movem_ea.mode == M68kEaMode::address_predec ||
            movem_ea.mode == M68kEaMode::address_postinc || movem_ea.mode == M68kEaMode::address_index8 ||
            movem_ea.mode == M68kEaMode::pc_index8;
        if (!representable) return "/* translation rejected: unsupported C4 operation */\n";
        auto routed = memory;
        routed.runtime_routing = true;
        routed.runtime_object = "runtime";
        // An ordinary MOVEM boundary can be selected either after its
        // adjacent LEA or directly through an interior compiled-entry alias.
        // Retain the one validated producer fact, but let the shared MOVEM
        // body use its immutable values only when the live architectural An
        // snapshot still equals the producer-derived base. A direct entry
        // with different live state therefore takes that same body's generic
        // routed path; no predecessor/resume token or second body is needed.
        if (!store) {
          const auto lea_fact = movem_lea_facts.find(provenance.source.address.value);
          if (lea_fact != movem_lea_facts.end()) {
            const auto order = m68k_movem_transfer_order(found->second->movem_register_mask,
                                                          M68kMovemTransferOrder::ascending);
            if (lea_fact->second->transfer_values.size() == order.size()) {
              routed.movem_transfer_access.assign(order.size(), M68kOperandAccess::resolved_constant);
              routed.movem_transfer_values = lea_fact->second->transfer_values;
              routed.movem_transfer_fold_base = lea_fact->second->resolved_base;
            }
          }
        }
        // movem_transfer's shared emitter (like every other kind never
        // integrated with the M68kMemoryEmissionContext::program_counter
        // field) advances PC through a literal `pc` identifier, exactly like
        // write_move above; the same `#define`/`#undef` bridge binds it to
        // `runtime->pc` for this block-local emission only.
        out << "#define pc runtime->pc\n"
            << emit_m68k_operation_c(*found->second, "runtime->d", "runtime->sr", "  ", &routed)
            << "#undef pc\n";
        break;
      }
      case M68kIrKind::return_from_subroutine: {
        const auto effect = m68k_operation_effect(*found->second);
        if (effect.stack != M68kStackEffectKind::pop_static_return || effect.stack_width != 4U ||
            effect.pc != M68kPcEffectKind::observed_stack_return)
          return "/* translation rejected: invalid C4 static return effect */\n";
        auto routed = memory;
        routed.runtime_routing = true;
        routed.runtime_object = "runtime";
        // SEG-007-T134 correction (ADR-0011 Decision §1): every retained
        // RTS validates the popped target against the SAME whole-program
        // continuation set, never only against the `return_to_continuation`
        // edges sourced at this particular RTS. The actual dispatch
        // destination remains the literal popped stack value
        // (`emit_m68k_operation_c`'s own `runtime_return_targets` handling,
        // unchanged), followed by the unchanged `genesis_dispatch`; no
        // target is selected statically here.
        const std::vector<std::uint32_t> return_target_values(runtime_return_target_set.begin(), runtime_return_target_set.end());
        routed.runtime_return_targets = return_target_values;
        if (routed.runtime_return_targets.empty()) return "/* translation rejected: invalid C4 static return edge */\n";
        out << emit_m68k_operation_c(*found->second, "runtime->d", "runtime->sr", "  ", &routed);
        break;
      }
      case M68kIrKind::return_from_exception: {
        // SEG-007-T047 / ADR-0020 §9: RTE. No static edge, no return-target
        // membership set -- the restored PC is a generated-runtime fact popped
        // from the exception frame by genesis_exception_return. Routed only so
        // the shared emitter binds runtime_source/runtime_provenance_helper.
        const auto effect = m68k_operation_effect(*found->second);
        if (effect.stack != M68kStackEffectKind::pop_exception_frame || effect.stack_width != 6U ||
            effect.pc != M68kPcEffectKind::observed_exception_return)
          return "/* translation rejected: invalid C4 exception-return effect */\n";
        auto routed = memory;
        routed.runtime_routing = true;
        routed.runtime_object = "runtime";
        out << emit_m68k_operation_c(*found->second, "runtime->d", "runtime->sr", "  ", &routed);
        break;
      }
      default:
        return "/* translation rejected: C4 prefix lacks retained resolver fact */\n";
      }
      out << "    { const uint32_t m68k_retirement_pc = runtime->pc; GenesisControlTransfer retired = "
          << retire_call_open(provenance.source.address.value, provenance.length.value, found->second->kind);
      out << *retirement_cycles;
      out << ", runtime->pc); if (retired.kind != GENESIS_CONTINUE_AT_PC || retired.next_pc != m68k_retirement_pc) return retired; }\n  }\n";
    }
    if (block_cut) {
      out << "}\n";
      continue;
    }
    out << "  GenesisControlTransfer transfer = {0};\n";
    for (const auto reached_address : reached_frontier_addresses)
      out << "  if (runtime->pc == UINT32_C(" << hex(reached_address, 8) << ")) return genesis_frontier_stop_"
          << frontier_stop_names.at(reached_address) << "(runtime);\n";
    out << "  transfer.kind = GENESIS_CONTINUE_AT_PC;\n"
        << "  transfer.next_pc = runtime->pc;\n"
        << "  return transfer;\n}\n";
  }
  // SEG-007-T246: supply the SAME already-computed whole-program
  // `runtime_return_target_set` (built above, ADR-0011 Decision §1) that
  // every ordinary CFG-rooted `return_from_subroutine` emission in this
  // same function already uses -- the exact existing authority, not a new
  // one, so an admitted AOT `RTS` candidate validates against the identical
  // membership set as any other RTS in this program.
  const std::vector<std::uint32_t> immutable_rom_aot_runtime_return_targets(
      runtime_return_target_set.begin(), runtime_return_target_set.end());
  // SEG-022-T008: in a sharded build admitted AOT entries are grouped, in ascending address order, into
  // owners of at most `aot_owner_max_entries` entries. An owner is one external function (one unit, so one
  // TU) whose `switch (runtime->pc)` enters exactly its own entries and fails closed for every other PC.
  std::map<Address, std::string> aot_owner_of;
  // SEG-022-T011 / ADR-0045: exact statically selected body helpers. Every AOT entry body is first lowered
  // by the unchanged owner (`emit_immutable_rom_aot_body` -> `emit_m68k_operation_c`) with its own
  // instruction provenance spelled as the pointer `genesis_aot_source`; nothing else about the entry is
  // abstracted. Entries whose complete lowered body text is byte-identical share ONE generated helper that
  // is exactly that text; each such entry calls it with a pointer to its own provenance constant. A body
  // used once stays inline. Identity is exact text equality of generated C -- never a runtime decision,
  // never a new semantic implementation -- and helper order/names are a pure function of the ascending
  // AOT address order, so output stays deterministic.
  constexpr std::string_view aot_source_symbol = "genesis_aot_source";
  const bool factor_aot_bodies = g_aot_body_factoring;
  std::vector<Address> aot_order;
  for (const auto &[address, entry] : *aot_entries) {
    (void)entry;
    if (!ordinary_compiled_owners.contains(address)) aot_order.push_back(address);
  }
  std::unordered_map<std::string, std::uint32_t> aot_body_ids;
  std::vector<const std::string *> aot_body_text;
  std::vector<std::uint32_t> aot_body_uses;
  std::vector<std::uint32_t> aot_entry_body;
  aot_entry_body.reserve(aot_order.size());
  if (factor_aot_bodies) for (const auto pc : aot_order) {
    auto inner = emit_immutable_rom_aot_body(*aot_entries->at(pc), immutable_rom_aot_runtime_return_targets,
                                             aot_unrepresented_exact_pcs[pc], {}, true, {},
                                             AotBodyFactoring{true, aot_source_symbol});
    if (inner.starts_with("/* translation rejected"))
      return inner;  // fail closed: an entry whose timing is unaccounted is never emitted
    const auto [slot, inserted] =
        aot_body_ids.try_emplace(std::move(inner), static_cast<std::uint32_t>(aot_body_text.size()));
    if (inserted) {
      aot_body_text.push_back(&slot->first);
      aot_body_uses.push_back(0U);
    }
    ++aot_body_uses[slot->second];
    aot_entry_body.push_back(slot->second);
  }
  // Helper names are assigned to shared bodies in first-use (ascending address) order.
  std::vector<std::string> aot_body_helper(aot_body_text.size());
  std::size_t aot_helper_count = 0U;
  for (std::uint32_t id = 0U; id < aot_body_text.size(); ++id) {
    if (aot_body_uses[id] < 2U) continue;
    std::ostringstream name;
    name << "genesis_aot_shared_" << std::setw(5) << std::setfill('0') << std::dec << aot_helper_count;
    aot_body_helper[id] = name.str();
    const bool uses_source = aot_body_text[id]->find(aot_source_symbol) != std::string::npos;
    const auto declaration = "GenesisControlTransfer " + aot_body_helper[id] + "(GenesisRuntime *runtime" +
                             (uses_source ? ", const GenesisInstructionProvenance *" + std::string(aot_source_symbol)
                                          : std::string()) + ")";
    ShardUnitScope unit(out, "shared", aot_helper_count, declaration);
    out << "static " << declaration << " {\n" << *aot_body_text[id] << "}\n";
    ++aot_helper_count;
  }
  // One entry's compound statement: a call of its shared helper, or the inline body with its provenance
  // pointer bound.
  const auto emit_aot_entry = [&](std::size_t index) {
    const auto pc = aot_order[index];
    if (!factor_aot_bodies) {
      // Unfactored reference form: the complete pre-T011 body with its own braces (no header).
      AotBodyFactoring unfactored{};
      unfactored.bare_block = true;
      return emit_immutable_rom_aot_body(*aot_entries->at(pc), immutable_rom_aot_runtime_return_targets,
                                         aot_unrepresented_exact_pcs[pc], {}, true, {}, unfactored);
    }
    const auto id = aot_entry_body[index];
    const auto &text = *aot_body_text[id];
    const bool uses_source = text.find(aot_source_symbol) != std::string::npos;
    const auto source = uses_source ? genesis_m68k_runtime_c_emitter().instruction_source(aot_entries->at(pc)->operation)
                                    : std::string();
    std::string result;
    if (!aot_body_helper[id].empty()) {
      result = "{ return " + aot_body_helper[id] + "(runtime" + (uses_source ? ", " + source : std::string()) + "); }\n";
    } else {
      result = "{\n";
      if (uses_source)
        result += "  const GenesisInstructionProvenance *const " + std::string(aot_source_symbol) + " = " + source + ";\n";
      result += text + "}\n";
    }
    return result;
  };
  if (sharded) {
    std::vector<std::size_t> pending;
    std::size_t owner_index = 0U;
    const auto flush_owner = [&]() {
      if (pending.empty()) return;
      std::ostringstream name;
      name << "genesis_aot_owner_" << std::setw(4) << std::setfill('0') << std::dec << owner_index++;
      const std::string owner_name = name.str();
      ShardUnitScope unit(out, "aot", aot_order[pending.front()],
                          "GenesisControlTransfer " + owner_name + "(GenesisRuntime *runtime)");
      out << "static GenesisControlTransfer " << owner_name << "(GenesisRuntime *runtime) {\n  switch (runtime->pc) {\n";
      for (const auto index : pending)
        out << "  case UINT32_C(" << hex(aot_order[index], 8) << "): goto genesis_aot_entry_" << std::uppercase
            << std::hex << std::setw(8) << std::setfill('0') << aot_order[index] << ";\n";
      out << "  default: return genesis_internal_dispatch_inconsistency_stop(runtime);\n  }\n";
      for (const auto index : pending) {
        out << "genesis_aot_entry_" << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
            << aot_order[index] << ": " << emit_aot_entry(index);
        aot_owner_of[aot_order[index]] = owner_name;
      }
      out << "}\n";
      pending.clear();
    };
    for (std::size_t index = 0; index < aot_order.size(); ++index) {
      pending.push_back(index);
      if (pending.size() >= aot_owner_max_entries) flush_owner();
    }
    flush_owner();
  } else
  for (std::size_t index = 0; index < aot_order.size(); ++index) {
    const auto address = aot_order[index];
    ShardUnitScope unit(out, "aot", address, genesis_unit_declaration("genesis_aot_", address));
    out << "static GenesisControlTransfer genesis_aot_" << std::uppercase << std::hex << std::setw(8)
        << std::setfill('0') << address << "(GenesisRuntime *runtime) " << emit_aot_entry(index);
  }
  // SEG-022-T003: the sorted compiled-entry table and its binary-search lookup form the one `entries` unit.
  if (sharded) shard_begin_unit(out, "entries", 0U, "GenesisCompiledEntry genesis_compiled_entry_lookup(uint32_t address)");
  std::vector<CompiledEntryBinding> compiled_entry_bindings;
  compiled_entry_bindings.reserve(emitted_code_addresses.size());
  for (const auto address : emitted_code_addresses) {
    std::ostringstream symbol;
    const auto ordinary = ordinary_compiled_owners.find(address);
    if (ordinary != ordinary_compiled_owners.end()) {
      symbol << "genesis_block_" << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
             << ordinary->second;
    } else {
      const auto aot = aot_entries->find(address);
      if (aot == aot_entries->end())
        return "/* translation rejected: compiled entry lacks generated body */\n";
      if (const auto owner = aot_owner_of.find(address); owner != aot_owner_of.end())
        symbol << owner->second;
      else
        symbol << "genesis_aot_" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << address;
    }
    compiled_entry_bindings.push_back({static_cast<std::uint32_t>(address), symbol.str()});
  }
  if (auto rejection = emit_compiled_entry_table(out, compiled_entry_bindings); !rejection.empty()) return rejection;
  if (sharded) shard_end_unit(out);
  out << "\nstatic GenesisControlTransfer genesis_dispatch(GenesisRuntime *runtime) {\n";
  out << "  { GenesisCompiledEntry entry = genesis_compiled_entry_lookup(runtime->pc);\n"
      << "    if (entry != NULL) return entry(runtime);\n"
      << "  }\n";
  // ADR 0013 Decision §6: one runtime->pc comparison per represented
  // frontier exit, uniform across every frontier class, immediately before
  // the internal-dispatch-inconsistency fallback. The existing per-block
  // tail arms above are unchanged and still short-circuit first; this arm
  // only re-identifies a dispatcher re-entry at an already-represented
  // frontier PC (unreachable through today's per-RTS list, but reachable
    // once ADR 0011 Decisions §§1-3's whole-program call-continuation set
    // widens beyond a single RTS's own outgoing edges) as its own precise
  // stop class rather than an internal invariant violation.
  for (const auto &[address, suffix] : frontier_stop_names)
    out << "  if (runtime->pc == UINT32_C(" << hex(address, 8) << ")) return genesis_frontier_stop_" << suffix
        << "(runtime);\n";
  out << "  return genesis_internal_dispatch_inconsistency_stop(runtime);\n}\n\n"
      << "GenesisControlTransfer genesis_bridge_dispatch(GenesisRuntime *runtime) {\n"
      << "  return genesis_dispatch(runtime);\n}\n";
  return {};  // success: text was streamed to `out`; non-empty return is a rejection
}

std::string emit_m68k_general_startup_runtime_c(const FrontendPartialProgram &partial) {
  std::ostringstream out;
  auto rejection = emit_m68k_general_startup_runtime_c_to(out, emit_genesis_runtime_c11_include(), partial);
  return rejection.empty() ? out.str() : rejection;
}

std::string emit_m68k_general_startup_bridge_c_to(std::ostream &sink, const FrontendPartialProgram &partial,
                                                   std::string_view rom_sha256, bool execution_history_hooks) {
  const ExecutionHistoryHooksScope hooks_scope(execution_history_hooks);
  if (!valid_bridge_rom_sha256(rom_sha256))
    return "/* translation rejected: invalid bridge ROM SHA-256 */\n";
  // SEG-007-T064: GENESIS_BRIDGE_REPORT_METADATA carries exactly one static
  // GenesisCpuDimensions value, compiled in once, that genesis_write_
  // sanitized_report consults whenever the *runtime-observed* stop_class
  // happens to be GENESIS_STOP_UNSUPPORTED_CPU_FORM -- it does not
  // distinguish which of several possible unsupported_cpu_form exits was
  // actually reached. With a bounded exit set, every retained
  // unsupported_cpu_form exit must therefore independently classify to the
  // exact same dimensions value, or this single static field cannot losslessly
  // represent whichever one the runtime actually selects; that ambiguity is
  // exactly as unrepresentable as a missing classification, so it is
  // rejected the same way rather than silently picking one exit's value.
  auto cpu_dimensions = std::optional<std::string_view>{"GENESIS_CPU_DIMENSIONS_NONE"};
  bool cpu_dimensions_set = false;
  for (const auto &frontier : partial.frontiers) {
    if (frontier.class_ != GenesisFrontierClass::unsupported_cpu_form) continue;
    if (!frontier.diagnostic.provenance) return "/* translation rejected: unrepresentable C5 CPU frontier */\n";
    const auto this_dimensions = c5_cpu_dimensions(classify_m68k_cpu_frontier(*frontier.diagnostic.provenance));
    if (!this_dimensions) return "/* translation rejected: unrepresentable C5 CPU frontier */\n";
    if (cpu_dimensions_set && *cpu_dimensions != *this_dimensions)
      return "/* translation rejected: unrepresentable C5 CPU frontier */\n";
    cpu_dimensions = this_dimensions;
    cpu_dimensions_set = true;
  }
  if (!cpu_dimensions) return "/* translation rejected: unrepresentable C5 CPU frontier */\n";
  // SEG-007-T077: independently re-verify every retained
  // M68kOwnedCartridgeRegionFact before embedding its backing data as
  // generated, build-time-constant C11 arrays -- the same "reject the whole
  // translation, never silently skip" discipline the movem-adjacent-LEA/
  // static-memory-fact precedent above already uses. Guarded: when no fact
  // is retained, nothing is emitted here (C11 forbids an empty array
  // initializer) and runtime.owned_regions/owned_region_count stay at
  // their zero default below, so a program that proves no region is
  // provably unaffected. See
  // docs/decisions/0006-generic-cartridge-data-region-ownership.md.
  std::ostringstream owned_region_data;
  std::ostringstream owned_region_table;
  std::size_t owned_region_count = 0U;
  {
    std::set<std::tuple<std::string, std::uint32_t, std::uint32_t, std::uint64_t, std::uint64_t>>
        owned_region_claims_seen;
    for (const auto &fact : partial.accepted_prefix.owned_cartridge_region_facts) {
      if (!valid_c4_owned_cartridge_region_fact(fact, partial.accepted_prefix.mapping_claims) ||
          !owned_region_claims_seen
               .emplace(fact.claim.name, fact.claim.target_begin.value, fact.claim.target_end.value,
                        fact.claim.image_begin.value, fact.claim.image_end.value)
               .second)
        return "/* translation rejected: invalid C4 owned cartridge region fact */\n";
      const auto array_name = "genesis_owned_region_data_" + std::to_string(owned_region_count);
      owned_region_data << "static const uint8_t " << array_name << "[] = {";
      for (std::size_t byte = 0; byte < fact.resolved_bytes.size(); ++byte)
        owned_region_data << (byte == 0 ? " " : ", ") << "UINT8_C(" << hex(fact.resolved_bytes[byte], 2) << ")";
      owned_region_data << " };\n";
      if (owned_region_count != 0U) owned_region_table << ",\n";
      owned_region_table << "  { UINT32_C(" << hex(fact.claim.target_begin.value, 8) << "), UINT32_C("
                          << hex(fact.claim.target_end.value, 8) << "), " << array_name << ", UINT32_C("
                          << hex(static_cast<std::uint32_t>(fact.resolved_bytes.size()), 8) << ") }";
      ++owned_region_count;
    }
  }
  // SEG-022-T002: stream the runtime body straight to `sink` behind the bridge
  // prelude; the complete program is never materialized in one string.
  // SEG-022-T003: a sharded sink gets the shared includes in the header and the report helpers in the main TU.
  std::string prelude;
  if (sharding_active(sink)) {
    shard_begin_header(sink);
    sink << emit_genesis_bridge_c11_shared_header_prelude();
    shard_end_header(sink);
    sink << emit_genesis_bridge_c11_main_prelude(rom_sha256, *cpu_dimensions);
  } else {
    prelude = emit_genesis_bridge_c11_prelude(rom_sha256, *cpu_dimensions);
  }
  if (auto rejection = emit_m68k_general_startup_runtime_c_to(sink, prelude, partial); !rejection.empty())
    return rejection;
  if (owned_region_count != 0U) {
    sink << owned_region_data.str() << "static const GenesisOwnedCartridgeRegion genesis_owned_cartridge_regions[] = {\n"
         << owned_region_table.str() << "\n};\n";
  }
  // SEG-007-T047 / ADR-0020 §6: emit the build-resolved IRQ6 autovector handler
  // entry only when its handler block was actually retained in the emitted
  // dispatch set (a member of accepted_prefix.static_blocks); otherwise the
  // runtime IRQ6 admission mechanism stays inert (fail-safe).
  std::string irq6_handler_hex;
  if (partial.accepted_prefix.irq6_handler_entry) {
    const auto handler = partial.accepted_prefix.irq6_handler_entry->value;
    for (const auto &block : partial.accepted_prefix.static_blocks)
      if (block.id.entry.value == handler) { irq6_handler_hex = hex(handler, 8); break; }
  }
  // SEG-007-T222 / ADR-0037: same "only when the handler block is actually
  // emitted" rule as the IRQ6 autovector above.
  std::string divide_by_zero_handler_hex;
  if (partial.accepted_prefix.divide_by_zero_handler_entry) {
    const auto handler = partial.accepted_prefix.divide_by_zero_handler_entry->value;
    for (const auto &block : partial.accepted_prefix.static_blocks)
      if (block.id.entry.value == handler) { divide_by_zero_handler_hex = hex(handler, 8); break; }
  }
  std::string privilege_violation_handler_hex;
  if (partial.accepted_prefix.privilege_violation_handler_entry) {
    const auto handler = partial.accepted_prefix.privilege_violation_handler_entry->value;
    for (const auto &block : partial.accepted_prefix.static_blocks)
      if (block.id.entry.value == handler) { privilege_violation_handler_hex = hex(handler, 8); break; }
  }
  // SEG-021-T019: the software-exception vector handlers, by the same "handler block emitted" rule.
  std::vector<std::pair<std::uint32_t, std::string>> software_exception_handler_hex;
  for (const auto &[software_vector, handler] : partial.accepted_prefix.software_exception_handler_entries)
    for (const auto &block : partial.accepted_prefix.static_blocks)
      if (block.id.entry.value == handler.value) {
        software_exception_handler_hex.emplace_back(software_vector, hex(handler.value, 8));
        break;
      }
  sink << emit_genesis_bridge_c11_main_open(
      hex(partial.accepted_prefix.startup_ingress->initial_ssp, 8),
      hex(partial.accepted_prefix.startup_ingress->entry.value, 8), irq6_handler_hex,
      divide_by_zero_handler_hex, privilege_violation_handler_hex, software_exception_handler_hex);
  if (g_execution_history_hooks) sink << "runtime.execution_history.detail_enabled = 1; runtime.m68k_checkpoint.enabled = 1; runtime.device_checkpoint.enabled = 1; ";
  if (owned_region_count != 0U) {
    sink << "  runtime.owned_regions = genesis_owned_cartridge_regions;\n";
    sink << "  runtime.owned_region_count = UINT32_C(" + std::to_string(owned_region_count) + ");\n";
  }
  sink << emit_genesis_bridge_c11_main_finish("genesis_bridge_dispatch");
  return {};
}

std::string emit_m68k_general_startup_bridge_c(const FrontendPartialProgram &partial,
                                                   std::string_view rom_sha256, bool execution_history_hooks) {
  std::ostringstream out;
  auto rejection = emit_m68k_general_startup_bridge_c_to(out, partial, rom_sha256, execution_history_hooks);
  return rejection.empty() ? out.str() : rejection;
}

std::string emit_m68k_general_startup_bridge_c_to(std::ostream &sink, const FrontendAnalysis &analysis,
                                                   std::string_view rom_sha256, bool execution_history_hooks) {
  const ExecutionHistoryHooksScope hooks_scope(execution_history_hooks);
  if (!valid_bridge_rom_sha256(rom_sha256))
    return "/* translation rejected: invalid bridge ROM SHA-256 */\n";
  if (!analysis.completion) {
    if (auto rejection = emit_m68k_general_startup_runtime_c_with_policy_to(
            sink, emit_genesis_bridge_c11_prelude(rom_sha256, "GENESIS_CPU_DIMENSIONS_NONE"), analysis,
            M68kGeneralStartupBlockEmissionPolicy::bridge_extended);
        !rejection.empty())
      return rejection;
    sink << emit_genesis_bridge_c11_main_open(hex(analysis.startup_ingress->initial_ssp, 8),
                                              hex(analysis.startup_ingress->entry.value, 8));
    if (g_execution_history_hooks) sink << "runtime.execution_history.detail_enabled = 1; runtime.m68k_checkpoint.enabled = 1; runtime.device_checkpoint.enabled = 1; ";
    sink << emit_genesis_bridge_c11_main_finish("genesis_dispatch");
    return {};
  }
  if (!analysis.startup_ingress || analysis.profile != M68kFrontendProfile::general_startup ||
      analysis.decoded.size() != analysis.ir.size() || analysis.static_blocks.empty())
    return "/* translation rejected: invalid synthetic completion analysis */\n";
  const auto &completion = *analysis.completion;
  const auto ssp = analysis.startup_ingress->initial_ssp;
  const auto byte_literal = [](std::uint32_t value) {
    return "UINT8_C(" + hex(value & UINT32_C(0xFF), 2) + ")";
  };
  if ((ssp & 1U) != 0U || !m68k_startup_ram_range_in_range(ssp, 4U))
    return "/* translation rejected: invalid synthetic completion stack slot */\n";
  std::map<Address, const M68kIrOperation *> operations;
  for (std::size_t index = 0; index < analysis.ir.size(); ++index) {
    if (!independently_decoded_and_lifted(analysis.decoded[index], analysis.ir[index]) ||
        !operations.emplace(analysis.ir[index].provenance.source.address.value, &analysis.ir[index]).second)
      return "/* translation rejected: invalid synthetic completion operation */\n";
  }
  std::map<Address, const M68kStaticBlock *> blocks;
  std::set<Address> bound;
  bool terminal_found = false;
  for (const auto &block : analysis.static_blocks) {
    if (block.id.entry.space != TargetAddressSpace::m68k_program || block.instructions.empty() ||
        block.instructions.front().source.address.value != block.id.entry.value ||
        !blocks.emplace(block.id.entry.value, &block).second)
      return "/* translation rejected: invalid synthetic completion block */\n";
    Address expected = block.id.entry.value;
    for (const auto &provenance : block.instructions) {
      const auto operation = operations.find(provenance.source.address.value);
      if (operation == operations.end() || !same_provenance(provenance, operation->second->provenance) ||
          provenance.source.address.value != expected || !bound.insert(expected).second)
        return "/* translation rejected: invalid synthetic completion block */\n";
      expected = static_cast<Address>(expected + provenance.length.value);
    }
    const auto *terminal = operations.at(block.instructions.back().source.address.value);
    if (same_provenance(block.instructions.back(), completion.terminal_rts)) {
      if (terminal->kind != M68kIrKind::return_from_subroutine) return "/* translation rejected: invalid completion terminal */\n";
      terminal_found = true;
    }
  }
  if (bound.size() != operations.size() || !blocks.contains(analysis.startup_ingress->entry.value) || !terminal_found)
    return "/* translation rejected: incomplete synthetic completion analysis */\n";
  std::map<Address, std::vector<const M68kStaticEdge *>> edges;
  for (const auto &edge : analysis.static_edges) {
    if (!operations.contains(edge.source_instruction.source.address.value) ||
        !same_provenance(edge.source_instruction, operations.at(edge.source_instruction.source.address.value)->provenance) ||
        edge.target.space != TargetAddressSpace::m68k_program || !blocks.contains(edge.target.value))
      return "/* translation rejected: invalid synthetic completion edge */\n";
    edges[edge.source_instruction.source.address.value].push_back(&edge);
  }
  std::ostringstream out;
  out << emit_genesis_bridge_c11_prelude(rom_sha256, "GENESIS_CPU_DIMENSIONS_NONE")
       << "GenesisControlTransfer genesis_static_stop(GenesisStopClass c, GenesisDiagnosticCategory d, const GenesisInstructionProvenance *p, uint8_t h, uint32_t a, GenesisAccessWidth w, GenesisAccessDirection x) { GenesisControlTransfer t = {0}; t.kind = GENESIS_STOP; t.stop.stop_class = c; t.stop.diagnostic_category = d; t.stop.provenance.has_instruction_provenance = 1U; t.stop.provenance.instruction = *p; t.stop.provenance.has_access = h; t.stop.provenance.access_address = a; t.stop.provenance.access_width = w; t.stop.provenance.access_direction = x; return t; }\n"
       << "void genesis_attach_route_provenance(GenesisRuntimeStop *s, const GenesisInstructionProvenance *p) { (void)s; (void)p; }\n"
       ;
  for (const auto &block : analysis.static_blocks) {
    out << "static GenesisControlTransfer genesis_block_" << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
        << block.id.entry.value << "(GenesisRuntime *runtime) {\n";
    for (const auto &provenance : block.instructions) {
      const auto *operation = operations.at(provenance.source.address.value);
      GenesisM68kEmissionContext memory{};
      std::vector<std::uint32_t> synthetic_return_targets;
  memory.execution_history_hooks = g_execution_history_hooks;
      memory.program_counter = "runtime->pc"; memory.address_registers = "runtime->a";
      memory.runtime_routing = true; memory.runtime_object = "runtime";
      if (operation->kind == M68kIrKind::call_general || operation->kind == M68kIrKind::bsr_call) {
        const auto &outgoing = edges[provenance.source.address.value];
        if (outgoing.size() != 1U || outgoing.front()->kind != M68kStaticEdgeKind::direct_call || !outgoing.front()->call)
          return "/* translation rejected: invalid synthetic completion call */\n";
        memory.continuation = outgoing.front()->call->continuation.value;
      } else if (operation->kind == M68kIrKind::return_from_subroutine) {
        for (const auto *edge : edges[provenance.source.address.value])
          if (edge->kind == M68kStaticEdgeKind::return_to_continuation) synthetic_return_targets.push_back(edge->target.value);
        if (same_provenance(provenance, completion.terminal_rts)) synthetic_return_targets.push_back(completion.sentinel_return_pc.value);
        memory.runtime_return_targets = synthetic_return_targets;
        if (memory.runtime_return_targets.empty()) return "/* translation rejected: invalid synthetic completion return */\n";
      } else if (operation->kind != M68kIrKind::write_moveq && operation->kind != M68kIrKind::write_clr &&
                 operation->kind != M68kIrKind::general_branch) {
        return "/* translation rejected: unsupported synthetic completion operation */\n";
      }
      out << emit_m68k_operation_c(*operation, "runtime->d", "runtime->sr", "  ", &memory);
    }
    if (same_provenance(block.instructions.back(), completion.terminal_rts))
      out << "  if (runtime->pc == UINT32_C(" << hex(completion.sentinel_return_pc.value, 8) << ")) { GenesisControlTransfer t = {0}; t.kind = GENESIS_COMPLETE; return t; }\n";
    out << "  { GenesisControlTransfer t = {0}; t.kind = GENESIS_CONTINUE_AT_PC; t.next_pc = runtime->pc; return t; }\n}\n";
  }
  out << "static GenesisControlTransfer genesis_dispatch(GenesisRuntime *runtime) {\n";
  for (const auto &block : analysis.static_blocks)
    out << "  if (runtime->pc == UINT32_C(" << hex(block.id.entry.value, 8) << ")) return genesis_block_" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << block.id.entry.value << "(runtime);\n";
  const auto slot = m68k_startup_ram_offset(ssp);
  out << "  return genesis_internal_dispatch_inconsistency_stop(runtime);\n}\n"
       << emit_genesis_bridge_c11_main_open(hex(ssp, 8), hex(analysis.startup_ingress->entry.value, 8))
       << (g_execution_history_hooks ? "runtime.execution_history.detail_enabled = 1; runtime.m68k_checkpoint.enabled = 1; runtime.device_checkpoint.enabled = 1; " : "")
       << "runtime.work_ram[" << slot << "] = " << byte_literal(completion.sentinel_return_pc.value >> 24U) << "; runtime.work_ram[" << slot + 1U << "] = " << byte_literal(completion.sentinel_return_pc.value >> 16U) << "; runtime.work_ram[" << slot + 2U << "] = " << byte_literal(completion.sentinel_return_pc.value >> 8U) << "; runtime.work_ram[" << slot + 3U << "] = " << byte_literal(completion.sentinel_return_pc.value) << "; "
       << emit_genesis_bridge_c11_main_finish("genesis_dispatch");
  sink << out.str();  // small synthetic completion program
  return {};
}

std::string emit_m68k_general_startup_bridge_c(const FrontendAnalysis &analysis,
                                                   std::string_view rom_sha256, bool execution_history_hooks) {
  std::ostringstream out;
  auto rejection = emit_m68k_general_startup_bridge_c_to(out, analysis, rom_sha256, execution_history_hooks);
  return rejection.empty() ? out.str() : rejection;
}

std::string emit_m68k_frontend_c(const FrontendAnalysis &analysis,const DirectFlowState &initial,std::uint64_t budget) {
  if (analysis.profile == M68kFrontendProfile::genesis_rom_startup) {
    if (budget != 5U)
      return "/* translation rejected: startup instruction budget must be exactly 5 */\n";
    if (!analysis.startup_ingress || analysis.decoded.size() != 5U || analysis.ir.size() != 5U ||
        analysis.static_blocks.size() != 3U || analysis.static_edges.size() != 2U ||
        analysis.static_frames.size() != 1U ||
        analysis.ir[0].kind != M68kIrKind::write_moveq ||
         analysis.ir[1].kind != M68kIrKind::write_move ||
         analysis.ir[2].kind != M68kIrKind::call_general ||
        analysis.ir[3].kind != M68kIrKind::return_from_subroutine ||
         analysis.ir[4].kind != M68kIrKind::write_move)
      return "/* translation rejected: incomplete static startup analysis */\n";
    std::ostringstream out;
    out << "/* Static lifted SEG-005 startup translation; no target image is present. */\n"
        << "#include <stdint.h>\n#include <inttypes.h>\n#include <stdio.h>\nint main(void) { uint32_t d[8] = {";
    for (std::size_t i = 0; i < initial.d.size(); ++i) { if (i) out << ','; out << "UINT32_C(" << hex(initial.d[i], 8) << ')'; }
    const auto &frame = analysis.static_frames.front().call;
    // SEG-007-T023: the register file is a full a[8] array (see
    // StartupState's own widening); only a[7] (the stack pointer) is seeded
    // here, matching this route's existing scope (no other selected kind
    // reads/writes a[0..6]).
    out << "}; uint8_t ram[65536] = {0}; uint32_t a[8] = {0}; a[7] = UINT32_C("
         << hex(analysis.startup_ingress->initial_ssp, 8)
         << "); uint32_t pc = UINT32_C(" << hex(analysis.startup_ingress->entry.value, 8)
         << "); uint16_t sr = UINT16_C(" << hex(initial.sr, 4) << "); uint32_t static_frame_ids[1] = {0U}; uint32_t static_frame_continuations[1] = {UINT32_C("
         << hex(frame.continuation.value, 8) << ")}; uint32_t static_frame_depth = 0U;\n";
    // Every operation's C lowering, including the absolute-memory and
    // static call/return forms with no SEG-003 equivalent, is owned by the
    // single shared emit_m68k_operation_c; this loop supplies only the
    // synthetic RAM/stack identifier configuration and the one already
    // discovered call continuation (T006), never per-operation semantics.
    const GenesisM68kEmissionContext memory{"ram", "a", "static_frame_ids", "static_frame_continuations",
                                             "static_frame_depth", frame.continuation.value, std::nullopt, 0U, {}, {}};
    for (std::size_t i = 0; i < analysis.ir.size(); ++i) {
      const auto &ir = analysis.ir[i];
      auto operation_memory = memory;
      // Fixed startup has already validated its only static MOVE source as
      // work RAM.  Supply that immutable region fact to the shared Batch-A
      // emitter; the adapter does not lower MOVE semantics itself.
      if (ir.kind == M68kIrKind::write_move &&
          ir.source_ea.mode == M68kEaMode::absolute_long) {
        operation_memory.test_operand_access = genesis_lowering_access(M68kAbsoluteOperandRegion::synthetic_work_ram);
      }
      switch (ir.kind) {
      case M68kIrKind::write_moveq:
       case M68kIrKind::write_move:
       case M68kIrKind::call_general:
      case M68kIrKind::return_from_subroutine:
        out << emit_m68k_operation_c(ir, "d", "sr", {}, &operation_memory); break;
      default: return "/* translation rejected: non-startup operation */\n";
      }
    }
    out << "printf(\"{\\\"d0\\\":\\\"%08\" PRIX32 \"\\\",\\\"d1\\\":\\\"%08\" PRIX32 \"\\\",\\\"a7\\\":\\\"%08\" PRIX32 \"\\\",\\\"pc\\\":\\\"%08\" PRIX32 \"\\\",\\\"sr\\\":\\\"%04\" PRIX32 \"\\\",\\\"stop_reason\\\":\\\"instruction_budget_exhausted\\\"}\\n\", d[0], d[1], a[7], pc, (uint32_t)sr); return 0; }\n";
    return out.str();
  }
  std::vector<M68kDirectFlowUnit> flow_units;
  flow_units.reserve(analysis.units.size());
  for (const auto &unit : analysis.units)
    flow_units.push_back({unit.ordinal, unit.id, unit.members, unit.entry_block, unit.provenance});
  return emit_m68k_structured_direct_flow_c(analysis.direct_flow, flow_units, initial, budget);
}

ImmutableRomAotBodyFactoringScope::ImmutableRomAotBodyFactoringScope(bool enabled)
    : previous_(g_aot_body_factoring) {
  g_aot_body_factoring = enabled;
}
ImmutableRomAotBodyFactoringScope::~ImmutableRomAotBodyFactoringScope() { g_aot_body_factoring = previous_; }

} // namespace segarecomp
