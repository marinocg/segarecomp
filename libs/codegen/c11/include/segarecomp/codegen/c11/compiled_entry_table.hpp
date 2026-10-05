#pragma once

// SEG-022-T009: compact generated compiled-entry ownership table.
//
// Generic and target-independent: guest addresses are opaque 32-bit keys and owners are opaque C
// symbol names. The emitted representation is
//   sorted guest-address table  ->  compact owner-id table  ->  owner-id -> host-symbol table
// so an owner reached by many guest addresses is named once instead of once per address. Lookup is an
// exact binary search that returns the owner's symbol or NULL; every address that is not a listed key
// (including boundaries and out-of-range values) fails closed with NULL.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "segarecomp/codegen/c11/translation_units.hpp"

namespace segarecomp {

// Smallest ordinary C integer width (8/16/32 bits) that can index `count` distinct owners, i.e. hold
// the ids 0 .. count-1. Deliberately no packed/odd widths.
[[nodiscard]] constexpr unsigned compiled_entry_owner_id_bits(std::size_t count) {
  if (count <= 0x100U) return 8U;
  if (count <= 0x10000U) return 16U;
  return 32U;
}

struct CompiledEntryBinding {
  std::uint32_t address = 0U;
  std::string owner_symbol;
  // SEG-036-T002: a direct entry has no owner. It names the statically selected exact generated helper
  // (uniform signature, see emit_compiled_entry_table_direct) and its packed provenance word initializer.
  std::string direct_helper{};
  std::string direct_provenance{};
};

struct CompiledEntryTableNames {
  std::string entry_type = "GenesisCompiledEntry";
  std::string lookup = "genesis_compiled_entry_lookup";
  std::string addresses = "genesis_compiled_entry_addresses";
  std::string owner_ids = "genesis_compiled_entry_owner_ids";
  std::string owners = "genesis_compiled_owners";
};

// `bindings` must be strictly ascending by address (the caller's sorted emitted-address set).
// Owner ids are assigned in order of first appearance, so the output is a pure function of the input.
// Returns "" on success or a rejection diagnostic when the input is not strictly ascending.
[[nodiscard]] inline std::string emit_compiled_entry_table(std::ostream &out,
                                                           const std::vector<CompiledEntryBinding> &bindings,
                                                           const CompiledEntryTableNames &names = {}) {
  for (std::size_t i = 1U; i < bindings.size(); ++i)
    if (bindings[i - 1U].address >= bindings[i].address)
      return "/* translation rejected: compiled entry addresses are not strictly ascending */\n";
  static constexpr char digits[] = "0123456789ABCDEF";
  const auto hex8 = [](std::uint32_t value) {
    std::string text = "0x00000000";
    for (int i = 0; i < 8; ++i) text[static_cast<std::size_t>(9 - i)] = digits[(value >> (4 * i)) & 0xFU];
    return text;
  };
  if (bindings.empty()) {
    out << "static " << names.entry_type << " " << names.lookup << "(uint32_t address) {\n"
        << "  (void)address;\n  return NULL;\n}\n";
    return {};
  }
  std::map<std::string_view, std::size_t> id_of;
  std::vector<std::string_view> owners;
  std::vector<std::size_t> ids;
  ids.reserve(bindings.size());
  for (const auto &binding : bindings) {
    const auto [it, inserted] = id_of.emplace(binding.owner_symbol, owners.size());
    if (inserted) owners.push_back(binding.owner_symbol);
    ids.push_back(it->second);
  }
  const unsigned bits = compiled_entry_owner_id_bits(owners.size());
  const std::string id_type = "uint" + std::to_string(bits) + "_t";
  const std::string id_macro = "UINT" + std::to_string(bits) + "_C";
  out << "static const uint32_t " << names.addresses << "[] = {\n";
  for (const auto &binding : bindings) out << "  UINT32_C(" << hex8(binding.address) << "),\n";
  out << "};\nstatic const " << id_type << " " << names.owner_ids << "[] = {\n";
  for (const auto id : ids) out << "  " << id_macro << "(" << std::to_string(id) << "),\n";
  out << "};\nstatic const " << names.entry_type << " " << names.owners << "[] = {\n";
  for (const auto owner : owners) out << "  " << owner << ",\n";
  out << "};\nstatic " << names.entry_type << " " << names.lookup << "(uint32_t address) {\n"
      << "  size_t low = 0U;\n"
      << "  size_t high = sizeof(" << names.addresses << ") / sizeof(" << names.addresses << "[0]);\n"
      << "  while (low < high) {\n"
      << "    const size_t middle = low + (high - low) / 2U;\n"
      << "    if (" << names.addresses << "[middle] < address) low = middle + 1U; else high = middle;\n"
      << "  }\n"
      << "  if (low < sizeof(" << names.addresses << ") / sizeof(" << names.addresses << "[0]) && "
      << names.addresses << "[low] == address) return " << names.owners << "[" << names.owner_ids << "[low]];\n"
      << "  return NULL;\n}\n";
  return {};
}

// ---- Direct-entry variant (SEG-036-T002 / ADR 0083) ---------------------------------------------------------------------
//
// Same sorted address table and owner-id table; an id below the legacy-owner count names an owner symbol as before, an id at or
// above it names `id - owner_count` in a table of statically selected exact helper functions. A direct entry's static
// provenance is one packed 64-bit word of `meta` (aligned with the address table; owner rows are zero):
//   bits 0-7 instruction length, bits 8-15 / 16-23 the two primary bytes, bits 24-63 the image offset.
// `genesis_compiled_entry_invoke` rebuilds from it exactly the `GenesisInstructionProvenance` value a legacy entry passed
// (CPU variant constant, source address = the entry's own PC). Nothing decodes guest code at run time: the generator chose
// each helper and packed each word; the tables only map a compiled address to that choice.
// Requires a shared header that declares `GenesisAotDirectHelper`, `GENESIS_NO_COMPILED_ENTRY`, `genesis_compiled_entry_find`,
// `genesis_compiled_entry_invoke` and `genesis_internal_dispatch_inconsistency_stop`.
struct CompiledEntryDirectNames {
  std::string stop_fn = "genesis_internal_dispatch_inconsistency_stop";
  std::string helpers = "genesis_aot_direct_helpers";
  std::string meta = "genesis_aot_direct_meta";
  std::string find = "genesis_compiled_entry_find";
  std::string invoke = "genesis_compiled_entry_invoke";
};

[[nodiscard]] inline std::string emit_compiled_entry_table_direct(std::ostream &out,
                                                                  const std::vector<CompiledEntryBinding> &bindings,
                                                                  const CompiledEntryTableNames &names = {},
                                                                  const CompiledEntryDirectNames &direct = {}) {
  if (bindings.empty()) return "/* translation rejected: direct entry table requires entries */\n";
  for (std::size_t i = 1U; i < bindings.size(); ++i)
    if (bindings[i - 1U].address >= bindings[i].address)
      return "/* translation rejected: compiled entry addresses are not strictly ascending */\n";
  static constexpr char digits[] = "0123456789ABCDEF";
  const auto hex8 = [](std::uint32_t value) {
    std::string text = "0x00000000";
    for (int i = 0; i < 8; ++i) text[static_cast<std::size_t>(9 - i)] = digits[(value >> (4 * i)) & 0xFU];
    return text;
  };
  std::map<std::string_view, std::size_t> owner_id_of;
  std::map<std::string_view, std::size_t> helper_id_of;
  std::vector<std::string_view> owners;
  std::vector<std::string_view> helpers;
  for (const auto &binding : bindings) {
    if (!binding.direct_helper.empty() != binding.owner_symbol.empty())
      return "/* translation rejected: compiled entry must be exactly one of owner or direct helper */\n";
    if (binding.direct_helper.empty()) {
      if (owner_id_of.emplace(binding.owner_symbol, owners.size()).second) owners.push_back(binding.owner_symbol);
    } else {
      if (binding.direct_provenance.empty())
        return "/* translation rejected: direct compiled entry lacks a provenance word */\n";
      if (helper_id_of.emplace(binding.direct_helper, helpers.size()).second) helpers.push_back(binding.direct_helper);
    }
  }
  // C forbids an empty initializer; a build whose entries are all direct keeps one never-selected placeholder owner.
  const bool placeholder_owner = owners.empty();
  const std::size_t owner_count = placeholder_owner ? 1U : owners.size();
  const unsigned bits = compiled_entry_owner_id_bits(owner_count + helpers.size());
  const std::string id_type = "uint" + std::to_string(bits) + "_t";
  const std::string owner_bound = std::to_string(owner_count) + "U";
  out << "static const uint32_t " << names.addresses << "[] = {\n";
  // Bare suffixed literals (not UINT*_C macros): a million-row table costs the compiler far less memory.
  for (const auto &binding : bindings) out << "  " << hex8(binding.address) << "u,\n";
  out << "};\nstatic const " << id_type << " " << names.owner_ids << "[] = {\n";
  for (const auto &binding : bindings)
    out << "  "
        << std::to_string(binding.direct_helper.empty() ? owner_id_of.at(binding.owner_symbol)
                                                        : owner_count + helper_id_of.at(binding.direct_helper))
        << ",\n";
  out << "};\nstatic const " << names.entry_type << " " << names.owners << "[] = {\n";
  if (placeholder_owner) out << "  NULL,\n";
  for (const auto owner : owners) out << "  " << owner << ",\n";
  out << "};\nstatic const GenesisAotDirectHelper " << direct.helpers << "[] = {\n";
  for (const auto helper : helpers) out << "  " << helper << ",\n";
  out << "};\nstatic const uint64_t " << direct.meta << "[] = {\n";
  for (const auto &binding : bindings)
    out << "  " << (binding.direct_helper.empty() ? std::string("0ULL") : binding.direct_provenance) << ",\n";
  out << "};\n"
      << "size_t " << direct.find << "(uint32_t address) {\n"
      << "  size_t low = 0U;\n"
      << "  size_t high = sizeof(" << names.addresses << ") / sizeof(" << names.addresses << "[0]);\n"
      << "  while (low < high) {\n"
      << "    const size_t middle = low + (high - low) / 2U;\n"
      << "    if (" << names.addresses << "[middle] < address) low = middle + 1U; else high = middle;\n"
      << "  }\n"
      << "  if (low < sizeof(" << names.addresses << ") / sizeof(" << names.addresses << "[0]) && "
      << names.addresses << "[low] == address) return low;\n"
      << "  return GENESIS_NO_COMPILED_ENTRY;\n}\n"
      << "GenesisControlTransfer " << direct.invoke << "(GenesisRuntime *runtime, size_t index) {\n"
      << "  const size_t id = " << names.owner_ids << "[index];\n"
      << "  if (id < " << owner_bound << ") return " << names.owners << "[id](runtime);\n"
      << "  { const uint64_t meta = " << direct.meta << "[index];\n"
      << "    const GenesisInstructionProvenance source = {GENESIS_CPU_MC68000, runtime->pc, meta >> 24,\n"
      << "      {(uint8_t)(meta >> 8), (uint8_t)(meta >> 16)}, (uint32_t)(meta & UINT64_C(0xFF))};\n"
      << "    return " << direct.helpers << "[id - " << owner_bound << "](runtime, &source, runtime->pc); }\n}\n"
      << "static GenesisControlTransfer genesis_direct_entry_stub(GenesisRuntime *runtime) {\n"
      << "  const size_t index = " << direct.find << "(runtime->pc);\n"
      << "  if (index == GENESIS_NO_COMPILED_ENTRY) return " << direct.stop_fn << "(runtime);\n"
      << "  return " << direct.invoke << "(runtime, index);\n}\n"
      << names.entry_type << " " << names.lookup << "(uint32_t address) {\n"
      << "  const size_t index = " << direct.find << "(address);\n"
      << "  if (index == GENESIS_NO_COMPILED_ENTRY) return NULL;\n"
      << "  { const size_t id = " << names.owner_ids << "[index];\n"
      << "    return id < " << owner_bound << " ? " << names.owners << "[id] : genesis_direct_entry_stub; }\n}\n";
  return {};
}

// ---- Chunked variant (SEG-008-T009): bounded per-translation-unit size for very large tables -------------------------
//
// A flat table in one TU (above) makes that TU's size, and so the compiler's peak memory, grow linearly with the number of
// entries plus the owner declarations it needs. The chunked form splits the same sorted bindings deterministically into
// consecutive chunks of at most `chunk_entries` entries. Chunk k is an independent unit of family `family` (one shard per
// chunk: the family must be declared with `compiled_entry_chunk_count(...)` shards and page_shift 0) holding its own
// owner declarations, key/owner-id/owner tables and an exact binary-search function. The glue (main TU) keeps only a sorted
// table of each chunk's first key and one exact binary search over it, then calls the chunk's lookup. Semantics are those of
// the flat table: an exact match returns the owner, everything else (below the first key, between chunks' keys, above the
// last) is NULL. The output is a pure function of the input; nothing depends on emission order or host.

struct CompiledEntryChunking {
  std::size_t chunk_entries = 0U;  // > 0; a table with <= chunk_entries entries stays flat
  std::string family = "entry";    // TranslationUnitFamily name for the chunk units
  // C declaration (without ';') of an owner symbol, written into every chunk TU that references it.
  std::function<std::string(std::string_view)> declare_owner;
};

[[nodiscard]] constexpr std::size_t compiled_entry_chunk_count(std::size_t entries, std::size_t chunk_entries) {
  return chunk_entries == 0U ? 1U : (entries + chunk_entries - 1U) / chunk_entries;
}

// True when emit_compiled_entry_table_chunked will emit chunk units for this stream/size. When it is false the caller must
// declare the owners itself (as for the flat table); when it is true the chunk units declare them.
[[nodiscard]] inline bool compiled_entry_table_is_chunked(std::ostream &out, std::size_t entries,
                                                          const CompiledEntryChunking &chunking) {
  return chunking.chunk_entries != 0U && entries > chunking.chunk_entries && sharding_active(out);
}

[[nodiscard]] inline std::string emit_compiled_entry_table_chunked(std::ostream &out,
                                                                   const std::vector<CompiledEntryBinding> &bindings,
                                                                   const CompiledEntryTableNames &names,
                                                                   const CompiledEntryChunking &chunking) {
  if (!compiled_entry_table_is_chunked(out, bindings.size(), chunking)) return emit_compiled_entry_table(out, bindings, names);
  for (std::size_t i = 1U; i < bindings.size(); ++i)
    if (bindings[i - 1U].address >= bindings[i].address)
      return "/* translation rejected: compiled entry addresses are not strictly ascending */\n";
  static constexpr char digits[] = "0123456789ABCDEF";
  const auto hex8 = [](std::uint32_t value) {
    std::string text = "0x00000000";
    for (int i = 0; i < 8; ++i) text[static_cast<std::size_t>(9 - i)] = digits[(value >> (4 * i)) & 0xFU];
    return text;
  };
  const std::size_t chunks = compiled_entry_chunk_count(bindings.size(), chunking.chunk_entries);
  const auto chunk_lookup = [&](std::size_t chunk) { return names.lookup + "_chunk_" + std::to_string(chunk); };
  for (std::size_t chunk = 0U; chunk < chunks; ++chunk) {
    const std::size_t begin = chunk * chunking.chunk_entries;
    const std::size_t end = std::min(bindings.size(), begin + chunking.chunk_entries);
    std::map<std::string_view, std::size_t> id_of;
    std::vector<std::string_view> owners;
    std::vector<std::size_t> ids;
    for (std::size_t i = begin; i < end; ++i) {
      const auto [it, inserted] = id_of.emplace(bindings[i].owner_symbol, owners.size());
      if (inserted) owners.push_back(bindings[i].owner_symbol);
      ids.push_back(it->second);
    }
    const unsigned bits = compiled_entry_owner_id_bits(owners.size());
    const std::string id_type = "uint" + std::to_string(bits) + "_t";
    const std::string id_macro = "UINT" + std::to_string(bits) + "_C";
    const std::string suffix = "_c" + std::to_string(chunk);
    const std::string addresses = names.addresses + suffix;
    ShardUnitScope unit(out, chunking.family, chunk, names.entry_type + " " + chunk_lookup(chunk) + "(uint32_t address)", false);
    for (const auto owner : owners) out << chunking.declare_owner(owner) << ";\n";
    out << "static const uint32_t " << addresses << "[] = {\n";
    for (std::size_t i = begin; i < end; ++i) out << "  UINT32_C(" << hex8(bindings[i].address) << "),\n";
    out << "};\nstatic const " << id_type << " " << names.owner_ids << suffix << "[] = {\n";
    for (const auto id : ids) out << "  " << id_macro << "(" << std::to_string(id) << "),\n";
    out << "};\nstatic const " << names.entry_type << " " << names.owners << suffix << "[] = {\n";
    for (const auto owner : owners) out << "  " << owner << ",\n";
    out << "};\nstatic " << names.entry_type << " " << chunk_lookup(chunk) << "(uint32_t address) {\n"
        << "  size_t low = 0U;\n  size_t high = sizeof(" << addresses << ") / sizeof(" << addresses << "[0]);\n"
        << "  while (low < high) {\n    const size_t middle = low + (high - low) / 2U;\n"
        << "    if (" << addresses << "[middle] < address) low = middle + 1U; else high = middle;\n  }\n"
        << "  if (low < sizeof(" << addresses << ") / sizeof(" << addresses << "[0]) && " << addresses
        << "[low] == address) return " << names.owners << suffix << "[" << names.owner_ids << suffix << "[low]];\n"
        << "  return NULL;\n}\n";
  }
  // Glue: the chunk directory (each chunk's first key, ascending) and one exact binary search over it.
  const std::string firsts = names.addresses + "_chunk_first";
  const std::string fns = names.owners + "_chunk_lookup";
  for (std::size_t chunk = 0U; chunk < chunks; ++chunk)
    out << names.entry_type << " " << chunk_lookup(chunk) << "(uint32_t address);\n";
  out << "static const uint32_t " << firsts << "[] = {\n";
  for (std::size_t chunk = 0U; chunk < chunks; ++chunk) out << "  UINT32_C(" << hex8(bindings[chunk * chunking.chunk_entries].address) << "),\n";
  out << "};\nstatic " << names.entry_type << " (*const " << fns << "[])(uint32_t) = {\n";
  for (std::size_t chunk = 0U; chunk < chunks; ++chunk) out << "  " << chunk_lookup(chunk) << ",\n";
  out << "};\nstatic " << names.entry_type << " " << names.lookup << "(uint32_t address) {\n"
      << "  size_t low = 0U;\n  size_t high = sizeof(" << firsts << ") / sizeof(" << firsts << "[0]);\n"
      << "  while (low < high) {\n    const size_t middle = low + (high - low) / 2U;\n"
      << "    if (" << firsts << "[middle] <= address) low = middle + 1U; else high = middle;\n  }\n"
      << "  if (low == 0U) return NULL;\n"
      << "  return " << fns << "[low - 1U](address);\n}\n";
  return {};
}

}  // namespace segarecomp
