#pragma once

// SEG-022-T009: compact generated compiled-entry ownership table.
//
// Generic and target-independent: guest addresses are opaque 32-bit keys and owners are opaque C
// symbol names. The emitted representation is
//   sorted guest-address table  ->  compact owner-id table  ->  owner-id -> host-symbol table
// so an owner reached by many guest addresses is named once instead of once per address. Lookup is an
// exact binary search that returns the owner's symbol or NULL; every address that is not a listed key
// (including boundaries and out-of-range values) fails closed with NULL.

#include <cstddef>
#include <cstdint>
#include <map>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

}  // namespace segarecomp
