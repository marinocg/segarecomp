#pragma once

// Genesis scenario composition: mapping-claim-backed instruction sources,
// Genesis memory routing, retained C4 facts, partial-frontier promotion, and
// legacy report compatibility. MC68000 discovery consumes this policy only
// through M68kStaticDiscoveryEnvironment; it never includes this header.

#include "segarecomp/cpu/m68k/static_program.hpp"
#include "segarecomp/device/sega/genesis/controller_io.hpp"
#include "segarecomp/machine/genesis/address_types.hpp"
#include "segarecomp/cpu/m68k/c4.hpp"
#include "segarecomp/recompiler/frontend.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace segarecomp {

struct GenesisResetImageReport;

enum class M68kFrontendProfile { direct_flow, genesis_rom_startup, general_startup };

// SEG-007-T164 / ADR-0023: one parsed, ROM-hash-verified record from an
// explicitly opted-in external hints interchange file
// (`kind = "logical_table_descriptor"`). This is trusted input from outside
// the pipeline's own generation-time proofs: every structural consequence
// (mapping, bounds, alignment, decode legality, target admission, the
// 256-member cap) is still independently re-derived, unchanged, by the
// existing ADR-0022/ADR-0009 machinery -- this record supplies only an
// alternate finite selector-position domain claim, never a bypass. See
// `docs/decisions/0023-*.md` for the full soundness contract and trust
// boundary.
struct GenesisLogicalTableDescriptorHint {
  std::uint32_t base_address{};
  std::uint32_t entry_width_bytes{};
  std::uint32_t stride_bytes{};
  std::uint32_t entry_count{};
  std::string provenance_tool;
  std::string provenance_tool_version;
  std::string provenance_timestamp;
  bool provenance_human_reviewed{false};
};

// SEG-007-T174 / ADR-0024: one parsed, ROM-hash-verified record from the same
// explicitly opted-in external hints interchange file
// (`kind = "code_entry_candidate"`). This is a PROPOSAL only, never authority:
// a candidate address becomes part of the seed set `discover_m68k_general_
// startup` walks (see `FrontendProgram::external_code_entry_candidates`
// below), but it still goes through the exact same independent
// decode/mapping/static-safety walk every other seed does, and only a
// candidate that survives that walk and is admitted into the accepted static
// prefix ever becomes a member of the generation-time `EmittedCodeAddressSet`
// Tier 2 dispatch checks against. See `docs/decisions/0024-*.md` for the full
// authority-boundary contract.
struct GenesisCodeEntryCandidateHint {
  std::uint32_t address{};
  std::string provenance_tool;
  std::string provenance_tool_version;
  std::string provenance_timestamp;
  bool provenance_human_reviewed{false};
};

// SEG-007-T203: an explicitly human-reviewed assertion that one ROM-bound,
// immutable-cartridge interval is a contiguous table of 4-byte absolute code
// pointers. The extent/type is trusted; the pointed-to values are not. They
// become ordinary ADR-0025 proposals and acquire authority only after the
// existing independent candidate walk admits them.
struct GenesisCodePointerTableDescriptorHint {
  std::uint32_t base_address{};
  std::uint32_t entry_count{};
  std::string provenance_tool;
  std::string provenance_tool_version;
  std::string provenance_timestamp;
  bool provenance_human_reviewed{false};
};

inline constexpr std::uint32_t genesis_code_pointer_table_max_entries = 256U;

// A hard bound on one explicitly requested immutable-ROM AOT interval.  It is
// local to this opt-in source and does not alter any static-discovery ceiling.
inline constexpr std::uint32_t genesis_test_immutable_rom_aot_range_max_candidates = 8192U;

[[nodiscard]] std::vector<GenesisCodePointerTableDescriptorHint>
parse_genesis_external_code_pointer_table_descriptors(
    std::string_view json_text, std::string_view expected_rom_sha256);

// SEG-007-T204 / ADR-0033 (re-homed by the 2026-09-11 operator correction):
// one parsed, ROM-hash-verified record of raw, explicitly NON-AUTHORITATIVE
// material read back from Ghidra's own "Create Address Tables" analyzer (see
// `ExportAddressTableCandidates.java`). This is NEVER an ADR-0023
// `logical_table_descriptor`, promoted or otherwise: `apply_genesis_
// address_table_corroboration` may promote it only into ordinary ADR-0025
// `code_entry_candidate` proposals (the same trust class ADR-0032's manual
// `code_pointer_table_descriptor` already uses), and only when it exactly
// agrees with a second, differently-sourced boundary signal (the existing
// unweakened ADR-0025 per-entry candidate validation walk, applied as a
// self-terminating prefix scan). A candidate that does not corroborate
// contributes nothing -- fail closed, never a guessed proposal set.
struct GenesisAddressTableCandidateHint {
  std::uint32_t base_address{};
  std::uint32_t entry_width_bytes{};
  std::uint32_t stride_bytes{};
  std::uint32_t entry_count{};
  std::string provenance_tool;
  std::string provenance_tool_version;
  std::string provenance_timestamp;
  bool provenance_human_reviewed{false};
};

// Parses `json_text` for `kind = "address_table_candidate"` records (same
// interchange file/schema as the other structured record parsers; may freely
// coexist with any other recognized kind in one file). Keeps only records
// whose `rom_sha256` exactly equals `expected_rom_sha256` and whose
// `base_address`/`entry_width_bytes` (must be 4)/`stride_bytes` (must be 4)/
// `entry_count` (1..`genesis_code_pointer_table_max_entries`, non-overflowing
// span)/`provenance` fields are all present and well-formed; a malformed or
// mismatched record is silently dropped, never a hard parse failure. Records
// sharing a `base_address` but disagreeing on `entry_count` conflict and are
// all dropped for that identity (mirrors
// `parse_genesis_external_code_pointer_table_descriptors`'s own conflict
// rule); the result is deterministically sorted by `base_address`.
[[nodiscard]] std::vector<GenesisAddressTableCandidateHint>
parse_genesis_external_address_table_candidates(
    std::string_view json_text, std::string_view expected_rom_sha256);

// SEG-007-T206: one MECHANICALLY DETECTED, explicitly NON-AUTHORITATIVE
// record noting that a recognized `(d8,PC,Xn)` word-Dn-indexed control
// instruction (JMP/JSR; the same EA shape ADR-0009/ADR-0024's
// `M68kUnprovenIndirectControlEaSet` already records when its finite
// index-value proof fails) has a table base with no matching ADR-0023
// `logical_table_descriptor` (manual, Ghidra-corroborated-4-byte, or a
// previously promoted 2-byte assertion). This kind deliberately carries NO
// `entry_count` field at all -- it never guesses the table's logical
// extent, only that one is missing -- and is never itself consumed by
// `M68kGeneralStartupEnvironment::logical_table_descriptor_hint` or any
// other trust path (see `detect_genesis_pc_relative_offset_table_extent_
// proposals` below and docs/decisions/0023-*.md). Promotion into an
// ordinary `logical_table_descriptor` happens only via the separate,
// bounded, one-time, development-time semantic-analysis judgment this
// task's own Scope Phase 3 describes -- never automatically from this
// record alone.
struct GenesisPcRelativeOffsetTableExtentProposalHint {
  std::uint32_t base_address{};
  std::uint32_t entry_width_bytes{2U};
  std::uint32_t stride_bytes{2U};
  std::string provenance_tool;
  std::string provenance_tool_version;
  std::string provenance_timestamp;
  bool provenance_human_reviewed{false};
};

// Mechanically derives one proposal per distinct recognized-but-index-
// value-unproven `(d8,PC,Xn)` word-Dn-indexed control site's table base
// (`control_ea.pc_base_address + control_ea.displacement`, the exact same
// formula `m68k_fold_immutable_offset_table` and `compute_indirect_target_
// set` already use) found in `unproven_indirect_control_ea_sets`, EXCEPT
// for a base that already has a matching entry (the identical
// `(base_address, entry_width_bytes == 2)` identity `logical_table_
// descriptor_hint` itself looks up) in `existing_descriptors`. A base
// whose computed table-base arithmetic underflows/overflows the 24-bit
// program address space, or whose control EA is An-indexed or long-sized
// (never this recognized word-Dn-indexed shape), is silently skipped --
// never a malformed proposal. Deterministically sorted and deduplicated by
// base address; every proposal shares the same supplied provenance triple
// and an honest `human_reviewed = false`. Never mutates either input
// vector; never itself trusted -- see the struct's own doc comment.
[[nodiscard]] std::vector<GenesisPcRelativeOffsetTableExtentProposalHint>
detect_genesis_pc_relative_offset_table_extent_proposals(
    const std::vector<M68kUnprovenIndirectControlEaSet> &unproven_indirect_control_ea_sets,
    const std::vector<GenesisLogicalTableDescriptorHint> &existing_descriptors,
    std::string provenance_tool, std::string provenance_tool_version, std::string provenance_timestamp);

// Serializes `proposals` as a JSON array of `kind =
// "pc_relative_offset_table_extent_proposal"` records (same interchange-
// file shape family as every other exported hint kind: `rom_sha256`,
// `base_address`, `entry_width_bytes`, `stride_bytes`, `provenance`), one
// object per proposal, in the exact order given. Never includes an
// `entry_count` field.
[[nodiscard]] std::string serialize_genesis_pc_relative_offset_table_extent_proposals(
    const std::vector<GenesisPcRelativeOffsetTableExtentProposalHint> &proposals,
    std::string_view rom_sha256);

// Parses `json_text` for `kind = "code_entry_candidate"` records (same
// interchange file/schema as `parse_genesis_external_hints`'s logical-table-
// descriptor records; the two kinds may freely coexist in one file, each
// parsed by its own reader). Keeps only records whose `rom_sha256` field
// exactly equals `expected_rom_sha256` and whose `address`/`provenance`
// fields are present and well-formed, exactly mirroring
// `parse_genesis_external_hints`'s fail-closed rules (a ROM-hash mismatch, a
// malformed/missing field, or a missing/malformed `provenance` object drops
// the record; a top-level JSON syntax error yields an empty list). The
// result is deterministically sorted by address and deduplicated (two
// records for the same address, agreeing or not, collapse to one entry --
// this candidate list carries no semantic content beyond "this address is
// worth an independent walk", so a duplicate is never a conflict).
[[nodiscard]] std::vector<GenesisCodeEntryCandidateHint> parse_genesis_external_code_entry_candidates(
    std::string_view json_text, std::string_view expected_rom_sha256);

// Parses `json_text` as the external hints interchange file (a single
// `kind = "logical_table_descriptor"` JSON object, or a JSON array of such
// objects), keeping only the records whose `rom_sha256` field exactly equals
// `expected_rom_sha256` and whose required fields (`base_address`,
// `entry_width_bytes`, `stride_bytes`, `entry_count`, both non-zero) are all
// present and well-formed. Any top-level JSON syntax error, any record with
// a mismatched `rom_sha256`, an unrecognized/missing `kind`, or a
// missing/malformed/zero required field is silently dropped -- never a hard
// parse failure -- so the caller always receives a (possibly empty) usable
// hint list and proceeds with the unmodified existing frontier for any site
// no surviving hint covers. This is a narrow, schema-scoped reader (object/
// array/string/number/bool/null only, no `\uXXXX` escapes); it is not a
// general-purpose JSON library and must not be reused outside this one
// interchange format.
[[nodiscard]] std::vector<GenesisLogicalTableDescriptorHint> parse_genesis_external_hints(
    std::string_view json_text, std::string_view expected_rom_sha256);
struct FrontendImage { std::string source_id; std::vector<std::uint8_t> bytes; std::uint64_t byte_length{}; };
struct M68kStartupIngress { M68kProgramAddress entry{}; std::uint32_t initial_ssp{}; };
struct FrontendProgram {
  FrontendProgram() = default;
  FrontendProgram(CpuVariant cpu, FrontendImage frontend_image, std::vector<MappingClaim> claims,
                  std::vector<M68kProgramAddress> entries)
      : cpu_variant(cpu), image(std::move(frontend_image)), mapping_claims(std::move(claims)),
        analysis_entries(std::move(entries)) {}
  CpuVariant cpu_variant{CpuVariant::mc68000};
  FrontendImage image{};
  std::vector<MappingClaim> mapping_claims{};
  std::vector<M68kProgramAddress> analysis_entries{};
  M68kFrontendProfile profile{M68kFrontendProfile::direct_flow};
  std::optional<M68kStartupIngress> startup_ingress;
  // ADR-0013 Decision §7 Phase B / §7c: additional ordered, build-time seeds
  // for discover_m68k_general_startup's per-seed independent walk
  // (ADR-0011 §4.1), beyond startup_ingress->entry (always round 1's own
  // fixed seed). Every entry here must already be either statically proven
  // or `runtime_confirmed` by an actual generated-native run (ADR-0011 §5) --
  // never `static_only` -- and this field carries no other seed-transport
  // meaning. Per ADR-0013 §7c the persisted confirmed-root set this vector
  // carries is a monotonically growing, checkpoint/resumable, TOTAL
  // cardinality with no fixed ceiling; only the Python driver's own
  // newly-promoted-root COUNT per invocation is bounded, by
  // m68k_discovery_max_seed_entries, entirely at the driver layer
  // (tools/genesis_startup_bridge.py's run_expansion_loop). This flat list
  // carries no distinction between roots persisted from an earlier
  // invocation and roots newly promoted this invocation, so
  // discover_m68k_general_startup does not and must not attempt to
  // re-enforce that per-invocation count as a total-size bound on this
  // vector; every seed here still receives its own fully independent,
  // per-root-bounded walk regardless of how many seeds there are.
  std::vector<M68kProgramAddress> runtime_confirmed_seeds{};
  struct CompletionContract { M68kProgramAddress terminal_rts_address{}; M68kProgramAddress sentinel_return_pc{}; };
  std::optional<CompletionContract> synthetic_completion;
  // Production-inert ADR-0038 negative-path controls. Honored only for the
  // project-authored `synthetic/` source-id namespace; ordinary/commercial
  // programs cannot populate these through any parser or hints format.
  struct Adr0038SyntheticTestControl {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> additional_exact_erased_unsafe_relations{};
    std::vector<std::uint32_t> additionally_unreachable_retained_entries{};
    std::optional<std::uint32_t> closure_round_ceiling{};
  };
  std::optional<Adr0038SyntheticTestControl> adr0038_synthetic_test_control{};
  // SEG-007-T164 / ADR-0023: zero or more explicitly opted-in, ROM-hash-
  // verified logical-table-descriptor annotations (see
  // `parse_genesis_external_hints`). Empty by default -- ordinary raw-ROM
  // recompilation that never opts into `--external-hints` never populates
  // this field, so `M68kGeneralStartupEnvironment::logical_table_descriptor_hint`
  // always returns `std::nullopt` and every existing proof path is
  // completely unaffected.
  std::vector<GenesisLogicalTableDescriptorHint> external_logical_table_descriptor_hints{};
  // SEG-007-T174 / ADR-0024: zero or more explicitly opted-in, ROM-hash-
  // verified Ghidra-or-equivalent whole-ROM code-entry candidates (see
  // `parse_genesis_external_code_entry_candidates`), sorted and deduplicated
  // by address. Empty by default -- ordinary raw-ROM recompilation that never
  // opts into `--external-hints` never populates this field, so
  // `discover_m68k_general_startup`'s seed set is exactly
  // `{reset entry} ∪ runtime_confirmed_seeds`, byte-for-byte unaffected by
  // this task. Every candidate here is folded into the seed set ahead of
  // `runtime_confirmed_seeds` (ADR-0024 Decision §4's demotion of Phase B to
  // fallback discovery) and receives the exact same independent per-root walk
  // as any other seed -- this field carries no authority of its own.
  std::vector<GenesisCodeEntryCandidateHint> external_code_entry_candidates{};
  // Empty by default. `apply_genesis_code_pointer_table_descriptors` reads
  // each accepted descriptor's complete immutable-cartridge interval at
  // generation time and adds its values only as ordinary candidate proposals.
  std::vector<GenesisCodePointerTableDescriptorHint> external_code_pointer_table_descriptors{};
  // SEG-007-T204 / ADR-0033 (re-homed by the 2026-09-11 operator correction):
  // zero or more explicitly opted-in, ROM-hash-verified, raw NON-
  // AUTHORITATIVE Ghidra "Create Address Tables" candidates (see
  // `parse_genesis_external_address_table_candidates`). Empty by default.
  // `apply_genesis_address_table_corroboration` reads this field at
  // generation time and, only when independent structural corroboration
  // succeeds, reads the candidate's immutable 4-byte pointer entries and
  // appends them to `external_code_entry_candidates` as ordinary ADR-0025
  // proposals (NEVER into `external_logical_table_descriptor_hints` --
  // ADR-0023's own consumer only ever looks up a 2-byte PC-relative
  // offset-word table shape, which this 4-byte mechanism cannot produce). A
  // candidate that does not corroborate contributes nothing (fail closed).
  std::vector<GenesisAddressTableCandidateHint> external_address_table_candidates{};
  // Explicit generic opt-in for independent, PC-keyed AOT representation.
  // These intervals are not discovery seeds and never create graph facts.
  struct ImmutableRomAotRange { std::uint32_t begin_address{}; std::uint32_t end_address{}; };
  std::vector<ImmutableRomAotRange> immutable_rom_aot_ranges{};
  bool immutable_rom_aot_enabled{false};
  // Test-only synthetic seam for exercising block partitioning independently
  // from admission. Production input parsing never populates this vector; it
  // is honored only for project-authored `synthetic/` images.
  std::vector<M68kProgramAddress> synthetic_semantic_partition_boundaries_for_test{};
};

// Applies descriptors atomically per interval. A zero/overflowing/oversized,
// partially mapped, ambiguously mapped, non-cartridge, or out-of-image extent
// contributes no proposal. Individual values are deliberately not filtered
// here: odd/unmapped/undecodable proposals must traverse and be rejected by
// the exact existing ADR-0025 validation/admission walk.
void apply_genesis_code_pointer_table_descriptors(FrontendProgram &program);

// Records one independently validated immutable-ROM enumeration interval.
// Analysis later decodes each aligned start directly into the separate AOT
// collection below; this deliberately never touches external candidates,
// roots, blocks, edges, frames, targets, or any other CFG authority.
[[nodiscard]] bool apply_genesis_immutable_rom_aot_range(
    FrontendProgram &program, std::uint32_t begin_address, std::uint32_t end_address);

// Production opt-in. Derives the complete finite source exclusively from all
// uniquely mapped, structurally valid immutable raw-cartridge claims. No
// caller-selected address or extent enters this operation.
[[nodiscard]] bool apply_genesis_immutable_rom_aot(FrontendProgram &program);

// SEG-007-T204 / ADR-0033 (re-homed by the 2026-09-11 operator correction):
// promotes zero or more `external_address_table_candidates` into ordinary
// `external_code_entry_candidates` PROPOSALS -- exactly the same trust class
// ADR-0032's `apply_genesis_code_pointer_table_descriptors` already uses,
// and reusing its exact entry-extraction/candidate-emission logic -- but
// ONLY when independent structural corroboration succeeds; see ADR-0033 for
// the full two-signal agreement rule. This function never reads or writes
// `external_logical_table_descriptor_hints`: ADR-0023's own consumer only
// ever looks up a 2-byte PC-relative offset-word table shape, which this
// 4-byte absolute-code-pointer mechanism structurally cannot produce, so
// promoting into that vector would never be reachable by that lookup.
// Fails closed (no proposals appended) for any candidate that does not
// corroborate. Every appended proposal still must independently survive the
// complete, unweakened ADR-0025 admission walk before it can affect
// generated output -- exact two-signal agreement is corroboration, not a
// universal proof of table identity (see ADR-0033 "Trust boundary").
// Idempotent: calling this more than once with an unchanged
// `external_address_table_candidates` does not duplicate entries (the
// self-terminating walk is deterministic and the shared sort+dedup pass
// collapses re-derived duplicates by address).
void apply_genesis_address_table_corroboration(FrontendProgram &program);
struct StaticEmissionUnit { std::uint32_t ordinal{}; std::string id; std::vector<BlockId> members; BlockId entry_block{}; std::vector<InstructionProvenance> provenance; };
enum class M68kStaticMemoryFactRole { source_read, destination_read, destination_write };
struct M68kStaticMemoryFact { InstructionProvenance operation{}; M68kStaticMemoryFactRole role{M68kStaticMemoryFactRole::source_read}; M68kProgramAddress address{}; M68kMemoryAccessWidth width{M68kMemoryAccessWidth::long_word}; M68kMemoryAccessDirection direction{M68kMemoryAccessDirection::read}; InstructionProvenance source_provenance{}; M68kAbsoluteOperandRegion region{}; std::uint32_t immutable_value{}; };
struct M68kMovemAdjacentLeaFact { InstructionProvenance producer{}; InstructionProvenance consumer{}; std::uint8_t address_register{}; std::uint32_t resolved_base{}; std::vector<std::uint32_t> transfer_values; };
struct M68kOwnedCartridgeRegionFact { MappingClaim claim{}; std::vector<std::uint8_t> resolved_bytes; };
struct FrontendAnalysis { M68kFrontendProfile profile{M68kFrontendProfile::direct_flow}; std::vector<M68kDecodedInstruction> decoded; std::vector<M68kIrOperation> ir; DirectFlowAnalysis direct_flow; std::vector<MappingClaim> mapping_claims; std::vector<StaticEmissionUnit> units; std::vector<M68kStaticBlock> static_blocks; std::vector<M68kStaticEdge> static_edges; std::vector<M68kStaticFrame> static_frames; std::vector<M68kStaticMemoryFact> static_memory_facts; std::vector<M68kMovemAdjacentLeaFact> movem_adjacent_lea_facts; std::vector<M68kOwnedCartridgeRegionFact> owned_cartridge_region_facts; std::vector<M68kIndirectTargetEaSet> indirect_target_ea_sets;
  struct ImmutableRomAotEntry {
    M68kDecodedInstruction decoded;
    M68kIrOperation operation;
    MappingClaim source_mapping;
  };
  // Independent executable identities. Provenance lives in `decoded` and
  // `operation`; `source_mapping` preserves the unique immutable byte owner.
  // Membership conveys no root/block/edge/frame/target fact.
  std::vector<ImmutableRomAotEntry> immutable_rom_aot_entries;
  // SEG-007-T174 / ADR-0024: the weaker Tier-2-eligible sibling of
  // `indirect_target_ea_sets` above -- see `M68kUnprovenIndirectControlEaSet`.
  std::vector<M68kUnprovenIndirectControlEaSet> unproven_indirect_control_ea_sets;
  std::optional<M68kStartupIngress> startup_ingress; struct CompletionRecord { InstructionProvenance terminal_rts; M68kProgramAddress sentinel_return_pc; }; std::optional<CompletionRecord> completion;
  // SEG-007-T047 / ADR-0020 §6: the build-time-resolved MC68000 level-6
  // interrupt autovector handler entry (the long word at mapped cartridge
  // image vector-table offset 0x78). Present only when general-startup
  // discovery resolved it and admitted the handler block into the emitted
  // dispatch set; it is a bounded static hardware discovery root entirely
  // outside ADR-0013 §7's seed set S.
  std::optional<M68kProgramAddress> irq6_handler_entry;
  // SEG-007-T222 / ADR-0037: the build-time-resolved MC68000 vector-5
  // (Zero Divide) handler entry (the long word at mapped cartridge image
  // vector-table offset 0x14), mirroring `irq6_handler_entry`'s own
  // resolution/ownership rule exactly -- present only when general-startup
  // discovery resolved it and admitted the handler block into the emitted
  // dispatch set; a bounded static hardware discovery root entirely outside
  // ADR-0013 §7's seed set S.
  std::optional<M68kProgramAddress> divide_by_zero_handler_entry;
  // SEG-007-T174 / ADR-0024: every `FrontendProgram::external_code_entry_
  // candidate` address that discovery actually admitted as a block entry
  // (i.e. every candidate that survived its own independent walk), in the
  // hints file's own sorted/deduplicated order. Like `irq6_handler_entry`,
  // each of these is a legitimate but entry-DISCONNECTED retained root (no
  // static edge from the reset-entry-reachable graph reaches it, by
  // construction -- a validated Ghidra candidate earns emission by surviving
  // its own walk, never by being reachable from the reset entry). Every
  // reachability walk that already seeds itself from `irq6_handler_entry`
  // (`runtime_frontier_eligible` here, and the C4 emission reachability walk
  // in libs/codegen/c11/src/frontend.cpp) also seeds itself from every entry in
  // this list, so a validated candidate's own block is retained/emitted
  // rather than silently pruned as unreachable. Empty by default -- ordinary
  // raw-ROM recompilation never populates it.
  std::vector<M68kProgramAddress> validated_code_entry_candidate_roots;
  // SEG-007-T183 / ADR-0028 §8 (semantic partition boundary vs. bounded
  // diagnostic frontier reporting): every validated destination the
  // aggregated multi-unit static graph can legitimately target independent
  // of the bounded diagnostic `UnresolvedFrontier` report -- the union of
  // admitted offline-unit entries, synthesized resolved-control-target unit
  // entries, synthesized fallthrough-continuation unit entries, validated
  // `validated_code_entry_candidate_roots`, `irq6_handler_entry` (when
  // present), and every Tier-1 finite indirect-target candidate already
  // proven in `indirect_target_ea_sets`. Carries only addresses -- no
  // instructions, edges, or blocks of its own -- and is derived entirely
  // from this same `FrontendAnalysis`'s own already-validated facts; it is
  // NOT a second parallel static program. A stitched edge whose target is a
  // member is a legitimate partition-boundary edge even when that address is
  // not itself represented in the bounded diagnostic frontier report. Empty
  // for a build with no `external_code_entry_candidates` (byte-identical to
  // the pre-T183 route).
  std::set<std::uint32_t> semantic_partition_boundary_addresses;
  // SEG-007-T180 / ADR-0026: normalized instrumentation proving that a broad
  // offline code-entry inventory was validated as bounded independent local
  // units STITCHED into one aggregated static program, rather than the
  // reset-entry walk recursively rediscovering downstream inventory code.
  // Numbers only -- never a raw address. Zeroed for a build with no
  // `external_code_entry_candidates` (byte-identical to the pre-T180 route).
  struct OfflineInventoryStitchMetrics {
    std::uint32_t offline_candidate_count{};
    std::uint32_t admitted_unit_count{};
    std::uint32_t rejected_fatal_probe_failure{};
    std::uint32_t rejected_walk_truncated_budget_cut{};
    std::uint32_t rejected_entry_undecodable{};
    std::uint32_t rejected_other{};
    std::uint32_t stitched_direct_edge_count{};
    std::uint32_t overlapping_unit_agreement_count{};
    std::uint32_t overlapping_unit_conflict_count{};
    std::uint32_t max_local_discovery_instructions{};
    std::uint32_t max_local_discovery_blocks{};
    std::uint32_t aggregate_local_discovery_instructions{};
    std::uint32_t aggregate_local_discovery_blocks{};
    // SEG-007-T180 / ADR-0026 §5, wired by SEG-007-T181 / ADR-0027 §6: the
    // final validated emitted block count and emitted code-address count,
    // populated from the C4 emission stage (libs/codegen/c11/src/frontend.cpp) via
    // the normalized `offline inventory emission` stderr line and merged back
    // by tools/genesis_startup_bridge.py. Zero when the inventory is empty.
    std::uint32_t emitted_block_count{};
    std::uint32_t emitted_code_address_count{};
    // SEG-007-T181 / ADR-0027: fallthrough-continuation partition
    // instrumentation. `stitched_fallthrough_continuation_edge_count` is the
    // aggregate of every Phase-2 pass's `stitched_fallthrough_continuation_edges`
    // (distinct from `stitched_direct_edge_count`).
    // `fallthrough_continuation_unit_count` is the number of continuation roots
    // synthesized to fixpoint; `continuation_synthesis_rounds` is the number of
    // pre-pass fixpoint rounds performed; `ceiling_trips_not_continuation_eligible`
    // counts reset/IRQ6 prefix-boundary-shaped `discovery_budget_exhausted`
    // issues whose address is NOT a continuation root -- the genuine open
    // control-edge frontier diagnostic, unchanged downstream. Numbers only.
    std::uint32_t stitched_fallthrough_continuation_edge_count{};
    std::uint32_t fallthrough_continuation_unit_count{};
    std::uint32_t continuation_synthesis_rounds{};
    std::uint32_t ceiling_trips_not_continuation_eligible{};
    // SEG-007-T182 / ADR-0028: count of units synthesized from
    // statically-resolved+validated direct control-transfer destinations reached
    // at a per-root instruction ceiling. Discovered by the same fixpoint
    // pre-pass as the continuation units, sharing its COUNT ceiling; routed
    // through the existing 4th-arg independent-unit boundary seam. Numbers only.
    // Zero for the empty-inventory / raw route.
    std::uint32_t synthesized_resolved_control_target_unit_count{};
    // SEG-007-T183 / ADR-0028 §8: semantic-partition-boundary-retention vs.
    // bounded-diagnostic-frontier-reporting instrumentation. Numbers only.
    // `semantic_partition_boundary_count` is
    // `semantic_partition_boundary_addresses.size()`.
    // `unresolved_semantic_frontier_count_before_bounding` is the complete,
    // untruncated deduplicated frontier-obligation count computed by
    // `recompiler_sort_dedup_and_project_frontiers`;
    // `diagnostic_frontier_count_after_bounding` is that projection's bounded
    // subset size; `residual_frontier_obligation_count` is its residual
    // count (the two always sum to
    // `unresolved_semantic_frontier_count_before_bounding`).
    // `retained_block_count_before_pruning`/`retained_block_count_after_
    // pruning` are `build_analysis`'s completed-blocks-only prefix's own
    // block count immediately before and after its iterative erase pass.
    // `ingress_retained` records whether the reset/startup entry block
    // survived that same erase pass. All zero/false for a build with no
    // `external_code_entry_candidates`.
    std::uint32_t semantic_partition_boundary_count{};
    std::uint32_t unresolved_semantic_frontier_count_before_bounding{};
    std::uint32_t diagnostic_frontier_count_after_bounding{};
    std::uint32_t residual_frontier_obligation_count{};
    std::uint32_t retained_block_count_before_pruning{};
    std::uint32_t retained_block_count_after_pruning{};
    bool ingress_retained{};
    // SEG-007-T190 / ADR-0031: bounded monotone fixed point over
    // {post-stitch Tier-1 finite-An proof -> late target representation (Stage
    // A) -> acceptance gate -> Tier-1-aware reachable-return resynthesis (Stage
    // B) -> retention}. `late_indirect_closure_rounds` is the number of rounds
    // executed (a converged closure always runs one final no-change round);
    // `late_indirect_target_unit_count` is the number of distinct late Tier-1
    // target candidate addresses admitted and merged as ordinary units by Stage
    // A across every round; `late_indirect_closure_converged` is false only when
    // the explicit round ceiling was reached with a still-changing round (the
    // build then fails closed with `discovery_budget_exhausted`, never silently
    // dropping a sound candidate/target/edge). All zero/false for a route that
    // never proves a late Tier-1 computed-JMP target.
    std::uint32_t late_indirect_closure_rounds{};
    std::uint32_t late_indirect_target_unit_count{};
    bool late_indirect_closure_converged{};
    // SEG-007-T213 / ADR-0026 §7: fixed-point Phase-1 admission instrumentation.
    // `admission_fixed_point_rounds` is the number of validation-pass rounds run
    // over the currently-scheduled/surviving candidate set until convergence:
    // exactly 0 when the initial candidate set is empty (zero-round no-op), and
    // >= 1 whenever it is non-empty (round 0's own pass always counts).
    // `admission_revalidation_count` is the number of per-candidate re-probes
    // performed across all rounds after round 0.
    // `admission_dependency_edge_count` is the size of the recorded
    // candidate-to-boundary dependency relation (0 if the whole-set fallback is
    // used instead of dependency tracking).
    // `rejected_after_dependency_removal_count` is the number of candidates that
    // were admitted in round 0 but rejected in a later round. Numbers only, no
    // raw address.
    std::uint32_t admission_fixed_point_rounds{};
    std::uint32_t admission_revalidation_count{};
    std::uint32_t admission_dependency_edge_count{};
    std::uint32_t rejected_after_dependency_removal_count{};
    // SEG-007-T214 / ADR-0028 §9: authoritative exact direct-control target
    // closure instrumentation. `authoritative_exact_target_count` is the size
    // of the closure obligation set (§9.1) extracted from the initial
    // completed-prefix erase-pass fixpoint. `authoritative_exact_targets_
    // already_represented` counts an obligated target that was already a
    // retained block entry before closure needed to act.
    // `authoritative_exact_targets_materialized` counts a target resolved via
    // §9.2 promotion (already decoded, non-entry) or via a fresh
    // `M68kStaticGraphWalker` unit walk, that ends the single rebuild-and
    // -reprune cycle as a retained block entry.
    // `authoritative_exact_targets_frontier_represented` counts a target that
    // received an actual emitted typed frontier-stop representation (the
    // materialize-then-reprune case, or an immediate validation-failure
    // fallback). `authoritative_exact_targets_rejected` counts a target that
    // could be neither materialized/promoted nor given a valid typed
    // frontier (the §9.1/§9.4 fail-closed case; nonzero here always
    // accompanies a `authoritative_exact_target_closure_exhausted`
    // rejection). `authoritative_exact_closure_rounds` is diagnostic-only
    // (§9.3 -- never itself a cap): 0 when the obligation set was empty or
    // fully already-satisfied without needing the rebuild-and-reprune cycle,
    // 1 once that single cycle ran. `authoritative_exact_targets_
    // unrepresented` must be 0 on every successfully generated program (a
    // nonzero value here without a fail-closed rejection is itself a
    // defect). Numbers only, no raw address.
    std::uint32_t authoritative_exact_target_count{};
    std::uint32_t authoritative_exact_targets_already_represented{};
    std::uint32_t authoritative_exact_targets_materialized{};
    std::uint32_t authoritative_exact_targets_frontier_represented{};
    std::uint32_t authoritative_exact_targets_rejected{};
    std::uint32_t authoritative_exact_closure_rounds{};
    std::uint32_t authoritative_exact_targets_unrepresented{};
    // ADR-0038: rooted representation-then-retention closure over the
    // authoritative erased-entry graph (SEG-007-T233/T234). `adr0038_graph_
    // node_count`/`adr0038_graph_edge_count` are the finite induced graph's
    // own size (0 when the completed-prefix transaction erased no candidate
    // entry -- the ordinary, unaffected case). Roots have zero in-degree,
    // leaves have zero out-degree, and maximum depth is the longest root-to-
    // node relation count. `adr0038_cyclic` is 1 only when
    // the acyclicity proof (closure step 1) failed, in which case every other
    // field below is 0 and the pre-ADR-0038 prefix/frontier is exactly
    // preserved. `adr0038_closure_rounds` counts fixed-point rebuilds (step
    // 6); `adr0038_peeled_component_count` and the deterministic sorted
    // `adr0038_peeled_component_sizes` record the actual whole unreachable
    // retained components atomically forbidden across those rebuilds (step
    // 5); component-only proposals removed with their owners are not retained
    // component nodes. `adr0038_peeled_node_count` is the deduplicated retained
    // component-node total.
    // `adr0038_retained_block_delta` is the actual total prefix block
    // growth over the exact pre-ADR-0038 prefix. `adr0038_ordinary_retained_
    // graph_node_count` is the distinct graph-state count; with typed and
    // unresolved nodes it sums to graph_node_count. `adr0038_typed_frontier_count` is the number
    // that ended as a validated destination-global `known_but_unemitted_
    // target` proposal; `adr0038_unresolved_node_count` is the remainder with
    // no truthful representation. `adr0038_ceiling_exhausted` is 1 only when
    // the architecture-owned round ceiling (`m68k_transitive_pruned_direct_
    // control_closure_round_ceiling`) was exhausted, which always accompanies
    // a full rollback to the pre-ADR-0038 prefix and frontier. Numbers only,
    // no raw address.
    std::uint32_t adr0038_graph_node_count{};
    std::uint32_t adr0038_graph_edge_count{};
    std::uint32_t adr0038_graph_root_count{};
    std::uint32_t adr0038_graph_leaf_count{};
    std::uint32_t adr0038_graph_max_depth{};
    std::uint32_t adr0038_cyclic{};
    std::uint32_t adr0038_closure_rounds{};
    std::uint32_t adr0038_peeled_component_count{};
    std::vector<std::uint32_t> adr0038_peeled_component_sizes{};
    std::uint32_t adr0038_peeled_node_count{};
    std::uint32_t adr0038_retained_block_delta{};
    std::uint32_t adr0038_ordinary_retained_graph_node_count{};
    std::uint32_t adr0038_typed_frontier_count{};
    std::uint32_t adr0038_unresolved_node_count{};
    std::uint32_t adr0038_ceiling_exhausted{};
  };
  OfflineInventoryStitchMetrics offline_inventory_stitch_metrics{}; };
// SEG-007-T185 / ADR-0028 §8 (boundary-retention-vs-representation batch): the
// single shared predicate for "this address is a validated semantic partition
// boundary the aggregated multi-unit static graph may legitimately terminate a
// straight-line run at and target with a representable edge of an existing edge
// kind". Consulted identically by build_analysis's straight-line block
// termination, its completed-prefix erase pass edge-safety check, and C4's own
// per-terminal straight-line-fallthrough edge-target acceptance
// (libs/codegen/c11/src/frontend.cpp), so all four decision paths agree on exactly
// the same set. Never used to admit a NEW address -- membership is derived
// entirely from this FrontendAnalysis's own already-validated facts.
inline bool is_semantic_partition_boundary_address(const FrontendAnalysis &analysis,
                                                   std::uint32_t address) {
  return analysis.semantic_partition_boundary_addresses.contains(address);
}

// Shared analysis/codegen boundary for one independently executable AOT
// operation. Most admitted forms need no CFG edge, call frame, return
// target, static memory fact, or fabricated target fact. `return_from_
// subroutine` is the sole exception (SEG-007-T246): ADR-0011 Decision §1
// already defines generated RTS semantics as a routed stack read validated
// against a deterministic, context-insensitive, whole-program continuation
// authority -- the exact same shared set every ordinary CFG-rooted RTS in
// the program already uses, never a per-RTS/per-callee/per-subroutine set.
// An isolated AOT candidate has no CFG/call/frame context of its OWN, but
// ADR-0011's own membership check needs none: it is a runtime safety net
// against the literal popped stack value, not a per-site reachability
// claim. `runtime_return_target_authority_available` tells this predicate
// whether the caller can actually supply that already-computed whole-
// program authority for the program currently being analyzed/emitted; when
// it cannot (no proven calls exist at all), RTS stays excluded exactly as
// before. Every other call/return/jump/exception-control form remains
// excluded until its own existing semantic owner can establish the same
// fact-free (or, for RTS, existing-whole-program-authority) contract.
inline bool m68k_operation_is_immutable_rom_aot_safe(const M68kIrOperation &operation,
                                                      bool runtime_return_target_authority_available) {
  const auto storage_free = [](const M68kEffectiveAddress &ea) {
    return ea.mode == M68kEaMode::data_register || ea.mode == M68kEaMode::address_register ||
           ea.mode == M68kEaMode::immediate;
  };
  const auto destination_register = [&] { return operation.destination_ea.mode == M68kEaMode::data_register; };
  switch (operation.kind) {
  case M68kIrKind::subtract_quick_long_d0:
  case M68kIrKind::branch_ne_short:
  case M68kIrKind::branch_always_short:
    // These compatibility-only direct-flow records do not have independent
    // general-startup bodies.  Keep all three outside immutable-ROM AOT: the
    // modern general-startup decoder/lifter owns equivalent operations.
    return false;
  case M68kIrKind::write_moveq:
  case M68kIrKind::general_branch:
  case M68kIrKind::dbcc_loop:
  case M68kIrKind::no_operation:
  case M68kIrKind::write_user_stack_pointer:
  case M68kIrKind::write_swap:
  case M68kIrKind::sign_extend_word:
  case M68kIrKind::sign_extend_long:
  case M68kIrKind::shift_rotate_register:
    return true;
  case M68kIrKind::load_effective_address:
    return operation.destination_ea.mode == M68kEaMode::address_register;
  case M68kIrKind::write_clr:
  case M68kIrKind::logical_not:
  case M68kIrKind::test_operand:
  case M68kIrKind::write_move:
  case M68kIrKind::write_movea:
    // SEG-021-T005: MOVE/MOVEA/CLR/NOT/TST family-level admission. Every
    // legal EA mode of these five mnemonics lowers through the shared C4
    // routed read/write primitives with no CFG edge, call frame, return
    // target or static memory fact: `emit_immutable_rom_aot_body` selects
    // runtime-routed access for absolute and d16(PC) reads, so no resolver
    // fact is required. Auto-updating operands use the operation-local
    // deferred address-register commit (c4-add-family-auto-update-commit-
    // contract.md), which returns from a failed routed access before any
    // architectural state changes; the emitter itself fails closed on any
    // shape it cannot lower (the C4 route-admission gate is unchanged).
    // Legality of the operand modes is owned by decode, not by this
    // predicate.
    return true;
  case M68kIrKind::negate_word:
    // Only the independently reached non-auto-updating d16(An) RMW form is
    // AOT-safe. Legal auto-updating forms use the normal routed lowering but
    // remain outside immutable-ROM AOT admission.
    return destination_register() || operation.destination_ea.mode == M68kEaMode::address_disp16;
  case M68kIrKind::read_status_register:
    return destination_register();
  case M68kIrKind::write_status_register:
  case M68kIrKind::write_condition_codes:
  case M68kIrKind::multiply_signed_word:
  case M68kIrKind::multiply_unsigned_word:
    return storage_free(operation.source_ea);
  case M68kIrKind::compare:
  case M68kIrKind::compare_immediate:
  case M68kIrKind::compare_address:
    // SEG-007-T249 (bounded family inventory, consuming T248's continuation
    // handoff): compare never writes back -- `emit_m68k_operation_c`'s
    // shared `compare`/`compare_immediate`/`compare_address` case
    // (libs/codegen/c11/src/m68k.cpp) only ever routes BOTH operands through
    // `m68k_emit_materialized_ea_read`/`m68k_emit_ea_read`, then performs a
    // condition-code update and a PC advance -- it never invokes
    // `m68k_emit_ea_write` at all, so it carries none of the destination
    // read-modify-write risk the add/subtract/logical family below still
    // has. Its admission can therefore be exactly as broad, independently
    // for EITHER operand, as `test_operand`'s own already-proven-safe list
    // (`address_disp16`, `address_index8`): neither mode ever mutates any
    // address register, and a failed routed read returns strictly before
    // the CCR-update/PC-advance statements that follow it, so a routed-read
    // failure can never partially mutate architectural state. `compare`/
    // `compare_address` (CMP/CMPA)'s own destination is always a fixed
    // Dn/An register (`m68k_decode_general_compare`'s `dst` is always
    // `{address_register-or-data_register, destination}` -- already
    // storage_free), and their source is legalized through `m68k_ea_
    // arithmetic_logical_indexed_source`, which legally reaches both
    // `address_disp16` and `address_index8` -- this is the actually
    // reachable shape this admission widens. `compare_immediate` (CMPI)'s
    // source is always the instruction-embedded immediate (already
    // storage_free), and its destination is legalized through `m68k_ea_
    // data_alterable`, which reaches `address_disp16` but never `address_
    // index8` -- so this predicate's own `address_index8`-for-destination
    // clause is a harmless, never-reached combination for CMPI specifically
    // (decode never produces it), symmetric with `test_operand`'s own
    // uniform treatment of both addressing-mode classes.
    {
      const auto compare_operand_safe = [&](const M68kEffectiveAddress &ea) {
        return storage_free(ea) || ea.mode == M68kEaMode::address_disp16 ||
               ea.mode == M68kEaMode::address_index8;
      };
      return compare_operand_safe(operation.source_ea) && compare_operand_safe(operation.destination_ea);
    }
  case M68kIrKind::add:
  case M68kIrKind::add_address:
  case M68kIrKind::add_immediate:
  case M68kIrKind::subtract:
  case M68kIrKind::subtract_address:
  case M68kIrKind::subtract_immediate:
  case M68kIrKind::logical_and:
  case M68kIrKind::logical_and_immediate:
  case M68kIrKind::logical_or:
  case M68kIrKind::logical_or_immediate:
  case M68kIrKind::exclusive_or:
  case M68kIrKind::exclusive_or_immediate:
    // SEG-007-T249 (bounded family inventory, ninth/tenth iteration of the
    // same bounded family T245 opened): the general (non-quick) ADD/SUB/
    // AND/OR/EOR read-modify-write memory-destination gap. Each of these
    // kinds' shared, plain (non-auto-updating) C4 lowering -- the fallback
    // body reached whenever the add-family's own deferred-address-commit
    // branch is not entered (add/add_address/add_immediate: the `if
    // (memory->runtime_routing && add_auto_kind && (add_auto_source ||
    // add_auto_destination))` guard around line 869; subtract/subtract_
    // address: the same shared body starting at line 776, reached whenever
    // `subtract_immediate_auto_destination` is false; logical_and/logical_
    // or/exclusive_or: the single shared body starting at line 989, which
    // already declines only a predec/postinc operand under runtime routing)
    // -- routes a plain, non-auto-updating destination through the exact
    // same `m68k_emit_ea_read` (RMW read side) then `m68k_emit_ea_write`
    // (write side, using the destination's own unchanged EA -- `address_
    // disp16` is never retargeted from predec/postinc, since it is neither
    // of those) pattern T248's own `add_quick`/`subtract_quick` carve-out
    // already independently verified is safe and fact-free: no
    // address-register mutation, no deferred-commit machinery, no
    // Q3-style aliasing hazard (recomputing the same non-mutating EA twice
    // -- once for the read, once for the write -- is idempotent since An
    // never changes), and no static fact of any kind. A routed-read failure
    // returns before the write is ever emitted; a routed-write failure
    // returns before the CCR-update/PC-advance statement that follows it,
    // so neither can partially mutate architectural state. Only `address_
    // disp16` is admitted here, unlike ADDQ/SUBQ's own sibling
    // `address_index8` carve-out above: this whole family's shared
    // "Dn,<ea>"-direction / immediate-form destination decode (`src/cpu/
    // m68k/decode.cpp`, every `m68k_decode_general_add`/`_subtract`/`_and`/
    // `_or`/`_eor` reverse/immediate-destination call site) is legalized
    // through `m68k_ea_data_alterable` or `m68k_ea_memory_alterable`,
    // neither of which ever includes `address_index8` -- unlike ADDQ/SUBQ's
    // own distinct `m68k_ea_addq_subq_destination` legal set. Admitting
    // `address_index8` here would therefore be an unreachable, unexamined
    // claim this family's own decode contract never actually produces; this
    // task admits only the addressing-mode class its own inventory confirms
    // decode can legally reach. `add_address`/`subtract_address`'s own
    // destination is always a plain address-register overwrite (never a
    // memory write at all -- decode.cpp fixes ADDA/SUBA's destination to
    // `address_register`), so this admission is a no-op for those two kinds
    // and changes nothing about their existing behavior. The source stays
    // storage_free-only: no memory-EA source shape has been examined for
    // this family in this task, so that side is unchanged and still
    // excluded pending its own independent proof. Every other memory
    // destination ((An), (An)+, -(An), absolute, pc_disp16, address_index8)
    // remains excluded pending its own independent proof.
    return storage_free(operation.source_ea) &&
           (storage_free(operation.destination_ea) ||
            operation.destination_ea.mode == M68kEaMode::address_disp16);
  case M68kIrKind::add_quick:
  case M68kIrKind::subtract_quick:
    // SEG-007-T248 (eighth iteration, same bounded family, same
    // architectural seam): ADDQ/SUBQ's own decode contract
    // (libs/cpu/m68k/src/decode.cpp) always overrides `source_ea` to
    // `M68kEaMode::immediate` (the instruction-embedded quick data field),
    // never a real EA read, so `storage_free(source_ea)` is unconditionally
    // true here and carries no additional risk regardless of destination.
    // The read-modify-write destination is admitted for `address_index8` on
    // the same reasoning as `write_move`/`test_operand` above: both
    // `add_quick`'s and `subtract_quick`'s shared C4 lowering
    // (libs/codegen/c11/src/m68k.cpp) route their plain (non-auto-updating)
    // memory destination through the exact same `m68k_emit_ea_read` (RMW
    // read side) then `m68k_emit_ea_write` (write side, using the
    // destination's own unchanged EA -- `address_index8` is neither
    // `address_predec` nor `address_postinc`, so it is never rewritten to
    // `address_indirect` the way those two auto-updating modes are) --
    // identical to every other admitted `address_index8` shape: no address-
    // register mutation, no deferred-commit machinery, no Q3-style aliasing
    // hazard (recomputing the same non-mutating EA twice, once for the read
    // and once for the write, is idempotent since neither An nor Xn ever
    // changes), and no static fact of any kind. A routed-read failure
    // returns before the write is ever emitted; a routed-write failure
    // returns before the CCR-update/PC-advance statement that follows it,
    // so neither can partially mutate architectural state. Every other
    // memory destination ((An), (An)+, -(An), d16(An)) remains excluded
    // pending its own independent proof, and the auto-updating (An)+/-(An)
    // destination path above is unaffected (this predicate is never
    // consulted for that already-distinct deferred-commit branch's own
    // shape, since `address_index8` never matches `address_predec`/
    // `address_postinc`).
    return storage_free(operation.source_ea) &&
           (storage_free(operation.destination_ea) ||
            operation.destination_ea.mode == M68kEaMode::address_index8);
  case M68kIrKind::bit_change:
  case M68kIrKind::bit_clear:
  case M68kIrKind::bit_set:
    if (storage_free(operation.source_ea) && storage_free(operation.destination_ea)) return true;
    // SEG-007-T246 (seventh iteration, same bounded family, same
    // architectural seam): BCHG/BCLR/BSET admit a plain `(An)` destination
    // (mode `address_indirect`; never `(An)+`/`-(An)`), the exact same
    // shape BTST's own carve-out below already proves safe for its
    // read-only destination, extended here to the write side. Unlike
    // `(An)+`/`-(An)`, plain `(An)` never mutates any address register in
    // either `m68k_emit_ea_read` or `m68k_emit_ea_write`'s shared
    // `m68k_emit_runtime_ea_address` helper (`case M68kEaMode::address_
    // indirect:` there emits no prelude/postlude at all) -- so the ordinary
    // routed read followed by the ordinary routed write needs no deferred-
    // commit machinery of any kind: there is no register state a failed
    // access could ever leave partially mutated. `(An)+`/`-(An)` stay fully
    // excluded: that same shared helper DOES mutate the live address
    // register directly in its predecrement prelude / postincrement
    // postlude (never a deferred local), so a routed-access failure there
    // could leave a real partial mutation -- an independent proof (or a
    // deferred-commit extension) this task does not attempt.
    return storage_free(operation.source_ea) && operation.destination_ea.mode == M68kEaMode::address_indirect;
  case M68kIrKind::bit_test:
    if (storage_free(operation.source_ea) && storage_free(operation.destination_ea)) return true;
    // SEG-007-T245 (third iteration, same bounded family, same architectural
    // seam): BTST is read-only by construction -- it never writes its
    // destination_ea (emit_m68k_operation_c's shared bit_test/bit_change/
    // bit_clear/bit_set case only ever writes back for the three mutating
    // kinds), so a plain `(An)` destination (no auto-increment/-decrement,
    // hence no address-register mutation of any kind, deferred or
    // otherwise) needs strictly less machinery than write_move's own
    // read-direction carve-out above: one ordinary routed read through the
    // same shared, already-proven `m68k_emit_ea_read`. SEG-007-T246
    // extends the same plain-`(An)`-only reasoning to the write side for
    // bit_change/bit_clear/bit_set above (their own destination write and
    // any auto-updating destination beyond plain `(An)` remain unproven for
    // this fact-free contract).
    return storage_free(operation.source_ea) && operation.destination_ea.mode == M68kEaMode::address_indirect;
  case M68kIrKind::return_from_subroutine:
    // SEG-007-T246: existing-authority integration, not a new architecture.
    // When the whole-program `runtime_return_target_set` (ADR-0011 Decision
    // §1) is available for this program, admitting RTS reuses its existing
    // semantic owner verbatim -- `emit_m68k_operation_c`'s unchanged
    // `return_from_subroutine` case, gated on `memory->runtime_routing`
    // exactly like every other carve-out in this family, performing the
    // same routed stack read, the same membership check against the same
    // shared set, the same fail-closed non-member path, and the same
    // dispatch-through-`genesis_dispatch` on membership. No new
    // `M68kStaticCall`, frame, `return_to_continuation` edge, caller/callee
    // relation, synthetic continuation, or second return mechanism is
    // introduced; the AOT candidate remains ownerless exactly as ADR-0039
    // already proved is sound for a CFG-frame-free RTS.
    return runtime_return_target_authority_available;
  case M68kIrKind::return_from_exception:
    return false;
  case M68kIrKind::jump_general:
    // SEG-007-T246 (eighth iteration, same bounded family, same
    // architectural seam): an unconditional JMP whose source EA is one of
    // the three compile-time-constant control-addressing forms (absolute.w,
    // absolute.l, d16(PC) -- exactly `m68k_is_statically_foldable_control_
    // ea`, the same predicate `m68k_operation_effect` itself already
    // consults to set `M68kPcEffectKind::direct_target`) needs no runtime
    // authority, memory access, or register mutation whatsoever:
    // `emit_m68k_operation_c`'s shared `jump_general`/`call_general` case
    // lowers a foldable JMP to nothing but `pc = UINT32_C(<constant>);`
    // (libs/codegen/c11/src/m68k.cpp) -- structurally identical in safety shape
    // to `general_branch`'s own unconditional admission above, and strictly
    // simpler than every memory-EA carve-out elsewhere in this family
    // (there is no EA to read, no address register to protect, no runtime
    // value to validate). The two runtime-only register-relative source
    // forms ((An), d16(An)) never set `direct_target` and stay excluded:
    // they need the same proven-candidate-membership machinery JSR's own
    // indirect forms already require (ADR-0009), an independent question
    // this task does not attempt for JMP.
    return m68k_is_statically_foldable_control_ea(operation.source_ea);
  case M68kIrKind::call_general:
    // SEG-007-T246 (ninth iteration, same bounded family, same
    // architectural seam): a foldable-target JSR needs no whole-program
    // authority at all -- unlike RTS's return-address validation, the
    // continuation address a CALL pushes is always this exact candidate's
    // own already-known provenance (source address + instruction length),
    // trivially computable per-candidate with no external fact and no
    // dependency on any other proven call/frame/edge.
    // `emit_immutable_rom_aot_body` sets `memory.continuation` from exactly
    // that provenance for every candidate, so the existing, unchanged
    // `call_general`/`bsr_call` lowering (libs/codegen/c11/src/m68k.cpp) pushes
    // the correct value through the same routed-write/alignment/range
    // checks its ordinary CFG-rooted form already uses, then assigns the
    // folded constant callee target -- structurally the JMP carve-out above
    // plus one self-contained, per-candidate routed stack write, never a
    // second call/return mechanism. The two runtime-only register-relative
    // source forms stay excluded, matching JMP's own scope; the callee
    // itself is validated independently at dispatch time, exactly like
    // every other represented-or-not target in this AOT model.
    return m68k_is_statically_foldable_control_ea(operation.source_ea);
  case M68kIrKind::bsr_call:
    // BSR's target is always foldable (a relative displacement, never a
    // runtime-only EA) -- the exact same reasoning as call_general above.
    return true;
  case M68kIrKind::push_effective_address:
  case M68kIrKind::link_frame:
  case M68kIrKind::unlink_frame:
  case M68kIrKind::movem_transfer:
  case M68kIrKind::shift_rotate_memory:
  case M68kIrKind::divide_signed_word:
  case M68kIrKind::divide_unsigned_word:
    return false;
  }
  return false;
}
enum class StartupBusKind { instruction_read, data_read, data_write, stack_read, stack_write };
struct StartupBusRecord { std::uint64_t ordinal{}; StartupBusKind kind{StartupBusKind::instruction_read}; M68kProgramAddress address{}; std::vector<std::uint8_t> bytes; std::string region; InstructionProvenance instruction{}; };
struct FrontendRejected { M68kFrontendProfile profile{M68kFrontendProfile::direct_flow}; DirectFlowDiagnostic category{}; std::optional<std::string> image_source_id; std::optional<std::string> supplied_image_source_id; std::optional<std::uint64_t> declared_image_byte_length; std::optional<std::uint64_t> actual_image_byte_length; std::optional<M68kProgramAddress> source_address; std::optional<MoveqImageOffset> image_offset; std::optional<InstructionProvenance> provenance; std::optional<std::uint64_t> available_bytes; std::optional<std::uint32_t> requested_length; std::optional<std::uint32_t> instruction_length; std::optional<std::vector<MappingClaim>> mapping_claims; RejectedDirectFlow direct{}; std::vector<StartupBusRecord> accesses; std::optional<ControllerIoAccessShape> controller_io_access_shape; };
// ADR 0013 Decision §6: discovery_prefix_boundary is appended last so no
// existing ordinal moves.
enum class GenesisFrontierClass { unsupported_cpu_form, unsupported_device_access, unsupported_memory_region, unresolved_indirect_target, known_but_unemitted_target, unsupported_interrupt_or_scheduling_event, discovery_prefix_boundary };
struct UnresolvedFrontier { GenesisFrontierClass class_{GenesisFrontierClass::unsupported_cpu_form}; FrontendRejected diagnostic; std::optional<M68kMemoryAccessRequest> access; };
// SEG-007-T064 introduced this as the smallest value that provably covered
// that task's own synthetic Checkpoint 4 multi-exit fixtures. ADR-0014
// Decision §3 raises it 4U -> 64U and re-characterizes it: unlike the
// discovery-resource ceilings just below (m68k_discovery_max_instructions/
// max_blocks), this is NOT a discovery-resource ceiling, is not sized by
// their doubling-search-with-headroom method, and never appears in any
// discovery_budget_exhausted signature (SEG-007-T087 confirmed this by code
// reading). It bounds only the count of `UnresolvedFrontier` records a
// promoted `FrontendPartialProgram` may carry -- i.e. the number of
// `genesis_frontier_stop_<addr>` functions and `runtime->pc` comparison arms
// the generated stop dispatcher contains -- a fixed partial-program
// representation-safety bound, not a completeness parameter. `64U` is
// justified structurally (roughly one third of `m68k_discovery_max_blocks` =
// 192U, rounded to a power of two -- a deliberate fraction, not a tight
// bound) and empirically (about 3.5x the SEG-007-T139 feasibility-spike-
// measured entry-connected frontier of 18 distinct exits on the authorized
// pinned route, a deliberate margin, not a fit). ADR-0014 fixes this value
// and forbids retuning it by route signature or doubling search; see
// docs/decisions/0014-entry-rooted-discovery-prefix-boundary-for-a-fragmented-ancestor.md
// Decision §3 for the full justification.
//
// SEG-007-T183 / ADR-0028 §8 re-characterizes this constant a second time:
// it is now a DIAGNOSTIC-REPORTING bound only, never a whole-program
// semantic-obligation capacity bound. A large validated multi-unit static
// graph may carry more than `m68k_discovery_max_frontier_exits` distinct
// semantic frontier obligations; `recompiler_sort_dedup_and_project_
// frontiers` represents every one of them in its `complete` set (what
// downstream C4 emission actually iterates) and exposes only the first
// `m68k_discovery_max_frontier_exits` deterministically-sorted entries plus a
// normalized residual count in the bounded diagnostic projection
// (`FrontendAnalysis::offline_inventory_stitch_metrics`'s
// `diagnostic_frontier_count_after_bounding` / `residual_frontier_
// obligation_count`). Exceeding it therefore no longer fails the whole
// translation by itself; only `recompiler_sort_dedup_and_bound_frontiers`
// (retained unchanged for any other caller) still rejects on overflow.
inline constexpr std::uint32_t m68k_discovery_max_frontier_exits = 64U;
using FrontendPartialProgram = RecompilerPartialProgram<FrontendAnalysis, UnresolvedFrontier>;
using FrontendResult = std::variant<FrontendAnalysis, FrontendPartialProgram, FrontendRejected>;
using StaticCallReturnResult = std::variant<StaticCallReturnDiscovery, FrontendRejected>;

// Bounded internal static-discovery engineering budgets (NOT Genesis hardware,
// timing, or console-behavior facts). Each is sized by the deterministic
// doubling-search-with-headroom method established by SEG-007-T054/T057 and
// batched by SEG-007-T087: double until the pinned Sonic route's current
// discovery_budget_exhausted signature stops recurring, continue at least one
// further doubled candidate for headroom, then adopt the larger confirmed value
// (or, when no further candidate fits the per-constant 32x-of-original ceiling,
// the largest ceiling-bounded candidate actually tested).
// m68k_discovery_max_instructions: original 8U (SEG-007-T054), ceiling 256U;
//   SEG-007-T087 -> 128U; SEG-007-T117 -> 256U (ceiling; SEG-007-T116's
//   MOVE-from-SR support re-exposed the T041 instruction-budget signature at a
//   larger reachable prefix, 128U still recurred it, 256U clears it, and no
//   further doubled candidate exists within the established ceiling).
inline constexpr std::uint32_t m68k_discovery_max_instructions = 256U;
// m68k_discovery_max_blocks: original 6U (SEG-007-T057), ceiling 32x6 = 192U;
//   SEG-007-T087 -> 48U; unchanged by SEG-007-T117. SEG-007-T118's MOVE <ea>,CCR
//   support re-exposed SEG-007-T055/T056's block-budget discovery_budget_exhausted
//   signature at a larger reachable prefix; SEG-007-T119 re-confirmed it from
//   scratch (deterministic, two byte-identical runs), ran the deterministic
//   doubling search (48U still recurred it; 96U clears it; 192U -- the established
//   32x-of-original ceiling, so no further headroom candidate is reachable -- also
//   clears it deterministically), and adopted the ceiling value 192U. At 192U the
//   pinned route advances to a qualitatively different pipeline boundary: a shared
//   MC68000 decode/lift/IR/C11 unsupported-instruction frontier (a MOVE-family
//   operation word with an indexed addressing mode), handed off to a CPU
//   decode/lift/IR/C11 successor, not another budget widen.
inline constexpr std::uint32_t m68k_discovery_max_blocks = 192U;
// SEG-007-T181 / ADR-0027 §4: the maximum number of synthesized
// fallthrough-continuation units one general-startup discovery may create
// across all fixpoint pre-pass rounds. This is a COUNT ceiling on partitioned
// linear-stretch work, NOT an instruction-budget raise: each continuation unit
// is still independently walked under the unchanged
// m68k_discovery_max_instructions per-root ceiling. A region that would need
// more continuation units than this fails the build closed with a normalized
// discovery_budget_exhausted diagnostic (no loop, no cap raise). Original
// value: one third of m68k_discovery_max_blocks = 192U, rounded to a power of
// two (64U), matching the m68k_discovery_max_frontier_exits sizing method;
// ADR-0027 forbids retuning it by route signature (i.e. no per-ROM/per-route
// special-cased value).
// SEG-007-T219: 64U -> 128U (doubling, the same generic, image-independent
// technique m68k_discovery_max_instructions/m68k_discovery_max_blocks
// themselves use for their own established-ceiling history above -- not a
// route-signature retune). SEG-007-T218's corrected, committed-data-
// conflict-free 197-record platforms/genesis/compat/ inventory resolves genuinely more
// real indirect jumps, making previously-unreachable straight-line code
// reachable; the fixpoint pre-pass needs 114 fallthrough-continuation + 14
// resolved-control-target units (128 total) to legitimately partition it,
// exceeding the original 64U count ceiling while the untouched per-root
// m68k_discovery_max_instructions ceiling is never approached (observed
// max_local_instr=249 < 256 at convergence, confirmed under both a decisive
// ephemeral unlimited-count sanity check and the adopted 128U value,
// deterministic and byte-identical across repeated runs). At 128U the
// existing ADR-0026/0027/0028 partitioning machinery converges the fixpoint
// pre-pass in 4 rounds with zero non-eligible ceiling trips
// (ceiling_trips_not_continuation_eligible=0) and the opted-in canonical
// one-shot route advances past static discovery entirely to a new,
// qualitatively different pipeline boundary: a CPU decode/lift/IR/C11
// unsupported-instruction frontier, handed off to that successor, not
// another budget widen.
// SEG-007-T223: 128U -> 256U by the same bounded generic doubling precedent.
// This remains one shared count ceiling for synthesized partition units, not a
// per-root instruction/block/seed/round budget. The focused synthetic boundary
// fixture exercises a partition requiring more than 128 units, deterministic
// convergence below this value, and a separate over-ceiling fail-closed case.
inline constexpr std::uint32_t m68k_fallthrough_continuation_unit_ceiling = 256U;
// ADR-0038 / SEG-007-T234: the architecture-owned closure-round ceiling for
// the rooted representation-then-retention closure over the authoritative
// erased-entry graph. Every productive round strictly grows the forbidden
// (peeled) node set (step 6 of the deterministic rooted transaction), so the
// number of rounds is already bounded by the finite induced graph's own node
// count; this ceiling is a defense-in-depth backstop, not a count this
// mechanism is expected to approach in practice. Exhausting it fails the
// WHOLE ADR-0038 transaction closed (full rollback to the pre-ADR-0038
// prefix and frontier), never a partial commit. Deliberately independent of
// every other discovery/unit/frontier ceiling above: raising one must never
// be read as raising another.
inline constexpr std::uint32_t m68k_transitive_pruned_direct_control_closure_round_ceiling = 64U;
inline constexpr std::uint32_t m68k_discovery_max_call_frame_depth = 2U;
// ADR-0013 Decision §7 Phase B / §7c: two fixed, production-owned,
// unsearched, image-independent build-time driver constants -- neither
// derived from any ROM and neither a retuning of
// m68k_discovery_max_instructions, which stays the sole per-seed-walk
// resource ceiling (ADR 0011 §4.1).
// m68k_discovery_max_seed_entries: per ADR-0013 §7c, this bounds ONLY the
// Phase B driver's own count of newly promoted runtime-confirmed roots in
// ONE driver invocation (enforced in tools/genesis_startup_bridge.py's
// run_expansion_loop). It is NOT, and must never become, a total-cardinality
// ceiling on the persisted confirmed-root set discover_m68k_general_startup
// receives across the lifetime of a discovery session -- that lifetime total
// is unbounded and monotonically growing by design (§7c). The numeric value
// is chosen to match m68k_discovery_max_frontier_exits's order of magnitude
// (see that constant's own doc comment; this ADR does not claim the two
// bounds interact beyond staying in the same order of magnitude).
// SEG-007-T167 (ADR-0013 Decision §7 amendment): raised 4U -> 5U together
// with m68k_expansion_max_rounds and the tools/genesis_startup_bridge.py
// driver mirrors, from re-confirmed executed evidence -- the authorized
// assisted pinned Phase-B route runs round 5 cleanly with no
// aggregation_conflict and reaches a new deterministic (byte-identical
// generated C across two runs) c4_lowering_gap terminal; a cap of 6
// reproduces the identical round-5 terminal, so 5 is the smallest cap that
// exposes the next real frontier.
// SEG-007-T170 (ADR-0013 Decision §7b amendment): raised 5U -> 8U together
// with m68k_expansion_max_rounds and the driver mirrors, from evidence that
// 5U saturated a second consecutive time on the identical resource-ceiling
// stop class after two real capability fixes; 6U reproduces the identical
// ceiling deterministically, 8U reaches a new deterministic
// c4_lowering_gap terminal at round 7, and 12U reaches the identical
// round-7 terminal. This is a headroom choice among the three evaluated
// candidates (6U/8U/12U), not a proof that 8 is the provably minimal
// sufficient cap -- intermediate values (e.g. 7) were never evaluated. See
// ADR-0013 Decision §7b for the full rationale. Kept synchronized with the
// driver.
// SEG-007-T172 (ADR-0013 Decision §7c amendment): this constant's numeric
// value is UNCHANGED (8U), but its meaning is corrected from a lifetime
// ceiling on the total confirmed-root set to a per-invocation
// newly-promoted-root batch budget; the total confirmed-root set persists
// and grows monotonically across driver invocations via
// tools/genesis_startup_bridge.py's `--checkpoint` mechanism. See ADR-0013
// §7c for the full corrected model.
inline constexpr std::uint32_t m68k_discovery_max_seed_entries = 8U;
// m68k_expansion_max_rounds: per ADR-0013 §7c, the maximum number of
// build/execute/expand rounds ONE Phase B driver invocation may perform
// before yielding a resumable `phase_b_batch_complete` outcome -- an
// independent outer bound that forces one invocation's own termination
// regardless of seed-set state, not a lifetime ceiling on how many rounds a
// discovery session may perform across resumed invocations.
// SEG-007-T167: raised 4U -> 5U with m68k_discovery_max_seed_entries (same
// re-confirmed executed evidence; see that constant's note above).
// SEG-007-T170: raised 5U -> 8U with m68k_discovery_max_seed_entries (same
// re-confirmed executed evidence; see that constant's note above).
// SEG-007-T172: meaning corrected to a per-invocation batch bound (§7c);
// numeric value unchanged.
inline constexpr std::uint32_t m68k_expansion_max_rounds = 8U;

struct FrontendOracleVector { std::string vector_id; std::string fixture_id; std::string image_sha256; CpuVariant cpu_variant{CpuVariant::mc68000}; M68kProgramAddress execution_entry{}; DirectFlowState initial{}; std::vector<std::uint64_t> block_instruction_counts; };
struct FrontendVectorAccepted { BlockProvenance block; MappingClaim mapping_claim; StaticEmissionUnit unit; DirectFlowExecution execution; };
struct FrontendVectorRejected { DirectFlowDiagnostic category{}; std::string vector_id; std::string fixture_id; std::string image_sha256; CpuVariant cpu_variant{CpuVariant::mc68000}; M68kProgramAddress execution_entry{}; std::string expected_fixture_id; std::string expected_image_sha256; CpuVariant expected_cpu_variant{CpuVariant::mc68000}; std::optional<BlockId> block; std::optional<InstructionProvenance> provenance; std::optional<std::vector<MappingClaim>> mapping_claims; };
using FrontendVectorResult = std::variant<FrontendVectorAccepted, FrontendVectorRejected>;

enum class StartupInstructionKind { moveq_d0, move_l_d0_absolute_long, move_l_absolute_long_d1, jsr_absolute_long, rts };
struct StartupInstruction { InstructionProvenance provenance{}; std::vector<std::uint8_t> raw_bytes; StartupInstructionKind kind{StartupInstructionKind::moveq_d0}; std::uint32_t operand{}; M68kDecodedInstruction decoded{}; M68kIrOperation operation{}; };
struct StartupState { std::array<std::uint32_t, 8> d{}; std::array<std::uint32_t, 8> a{}; std::uint16_t sr{}; M68kProgramAddress pc{}; };
struct StartupBoundary { std::uint64_t ordinal{}; StartupState state{}; std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> ram_bytes; std::optional<std::uint64_t> bus_through_ordinal; std::string stop_reason{"continue"}; };
struct StartupReturn { InstructionProvenance source{}; M68kProgramAddress target{}; M68kStaticCall call{}; };
struct StartupExecution { StartupState initial_state{}; std::vector<StartupInstruction> instructions; std::vector<StartupBusRecord> accesses; std::vector<StartupBoundary> boundaries; std::vector<M68kStaticCall> calls; std::vector<StartupReturn> returns; StartupState final_state{}; std::string stop_reason; };
struct StartupFailure { std::uint64_t ordinal{}; std::string category; M68kProgramAddress attempted_source{}; std::optional<MoveqImageOffset> image_offset; std::optional<InstructionProvenance> complete_instruction; std::optional<std::vector<std::uint8_t>> complete_instruction_raw_bytes; std::optional<InstructionProvenance> primary_provenance; std::uint64_t retained_boundary_ordinal{}; std::optional<std::uint64_t> bus_through_ordinal; std::uint64_t available_bytes{}; std::uint32_t requested_length{}; std::optional<std::uint32_t> effective_address; std::optional<std::pair<std::uint32_t, std::uint32_t>> attempted_range; std::optional<std::string> region; std::optional<std::uint32_t> a7; std::optional<M68kStaticCall> call; std::optional<std::uint32_t> expected_continuation; std::optional<std::uint32_t> observed_return_target; std::vector<StartupBusRecord> accesses; std::vector<StartupBoundary> boundaries; };
struct StartupExecutionTestContext { std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> ram_bytes; std::vector<M68kStaticCall> static_call_frames; };
using StartupResult = std::variant<StartupExecution, StartupFailure>;

[[nodiscard]] FrontendResult analyze_m68k_frontend(const FrontendProgram &program);
[[nodiscard]] StaticCallReturnResult discover_m68k_static_call_return(const FrontendProgram &program, const M68kDecodedInstruction &call_decoded, const M68kDecodedInstruction &return_decoded);
[[nodiscard]] FrontendResult discover_m68k_general_startup(const FrontendProgram &program);
[[nodiscard]] std::string format_m68k_general_startup_result(const FrontendAnalysis &analysis);
[[nodiscard]] std::string format_m68k_frontend_result(const FrontendResult &result);
[[nodiscard]] FrontendVectorResult validate_and_execute_m68k_frontend_vector(const FrontendAnalysis &analysis, const FrontendOracleVector &vector, std::string_view expected_fixture_id, std::string_view expected_image_sha256);
[[nodiscard]] StartupResult execute_m68k_frontend_startup(const FrontendAnalysis &analysis, StartupState initial, const StartupExecutionTestContext &test_context = {});
[[nodiscard]] std::string format_genesis_rom_startup_result(const StartupResult &result);

// Genesis command workflows supply validated image/reset inputs; these helpers
// own the literal scenario composition rather than the CLI.
[[nodiscard]] StartupState make_genesis_reset_startup_state(const GenesisResetImageReport &reset);
[[nodiscard]] FrontendProgram make_genesis_reset_startup_program(std::vector<std::uint8_t> bytes,
                                                                   const StartupState &initial,
                                                                   bool general_startup);
[[nodiscard]] std::optional<FrontendProgram> make_genesis_bridge_startup_program(
    std::vector<std::uint8_t> bytes, std::uint32_t mapping_base, std::uint32_t entry,
    std::optional<std::pair<std::uint32_t, std::uint32_t>> synthetic_completion);
// The canonical bridge preparation owns the Genesis reset handoff: it maps
// the complete input at cartridge base zero and uses the validated reset PC
// and SSP as the execution ingress.  Explicit synthetic preparations use the
// overload above so their mapping and entry remain independently stated.
[[nodiscard]] std::optional<FrontendProgram> make_genesis_reset_bridge_startup_program(
    std::vector<std::uint8_t> bytes, const GenesisResetImageReport &reset,
    std::optional<std::pair<std::uint32_t, std::uint32_t>> synthetic_completion);

} // namespace segarecomp
