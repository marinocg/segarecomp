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
