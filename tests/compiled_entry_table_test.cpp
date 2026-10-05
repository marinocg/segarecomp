// SEG-022-T009: compact compiled-entry table: width selection at count boundaries, and exact lookup
// equivalence (every valid, boundary, neighbouring and out-of-range address) of the emitted C against a
// reference std::map, compiled as strict C11.
#include "segarecomp/codegen/c11/compiled_entry_table.hpp"
#include "segarecomp/codegen/c11/translation_units.hpp"

#include <map>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

using namespace segarecomp;

namespace {
int failures = 0;
void check(bool ok, const char *what) {
  if (!ok) { std::cerr << "FAIL: " << what << "\n"; ++failures; }
}

void check_widths() {
  check(compiled_entry_owner_id_bits(0U) == 8U, "0 owners");
  check(compiled_entry_owner_id_bits(1U) == 8U, "1 owner");
  check(compiled_entry_owner_id_bits(256U) == 8U, "256 owners fit ids 0..255");
  check(compiled_entry_owner_id_bits(257U) == 16U, "257 owners need 16 bits");
  check(compiled_entry_owner_id_bits(65536U) == 16U, "65536 owners fit ids 0..65535");
  check(compiled_entry_owner_id_bits(65537U) == 32U, "65537 owners need 32 bits");
}

// `owners` distinct owners over `entries` sorted addresses (owner = index % owners, so owners repeat).
void check_lookup(const std::filesystem::path &dir, const std::string &name,
                  std::vector<std::uint32_t> addresses, std::size_t owners, unsigned expected_bits) {
  std::vector<CompiledEntryBinding> bindings;
  for (std::size_t i = 0; i < addresses.size(); ++i)
    bindings.push_back({addresses[i], "owner_" + std::to_string(i % owners)});
  std::ostringstream c;
  c << "#include <stdint.h>\n#include <stddef.h>\n#include <stdio.h>\ntypedef int (*GenesisCompiledEntry)(void);\n";
  const std::size_t distinct = std::min(owners, addresses.size());
  for (std::size_t i = 0; i < distinct; ++i) c << "static int owner_" << i << "(void) { return " << i << "; }\n";
  check(emit_compiled_entry_table(c, bindings).empty(), "emit");
  check(c.str().find("uint" + std::to_string(expected_bits) + "_t genesis_compiled_entry_owner_ids") != std::string::npos,
        "owner id width");
  std::set<std::uint32_t> probes = {0U, 1U, 0xFFFFFFFFU, 0xFFFFFFFEU, 0x00FFFFFFU, 0x01000000U};
  for (const auto a : addresses) { probes.insert(a); probes.insert(a + 1U); probes.insert(a - 1U); }
  c << "int main(void) {\n  static const uint32_t probes[] = {\n";
  for (const auto p : probes) c << "    UINT32_C(" << p << "),\n";
  c << "  };\n  for (size_t i = 0; i < sizeof probes / sizeof probes[0]; ++i) {\n"
    << "    GenesisCompiledEntry e = genesis_compiled_entry_lookup(probes[i]);\n"
    << "    printf(\"%u %d\\n\", (unsigned)probes[i], e == NULL ? -1 : e());\n  }\n  return 0;\n}\n";
  // Portable split: this program only writes the generated C and the expected lookup answers; the
  // Python driver compiles (strict C11) and runs each source with argument-list subprocesses.
  std::ofstream(dir / (name + ".c")) << c.str();
  std::map<std::uint32_t, long> expected;
  for (std::size_t i = 0; i < addresses.size(); ++i) expected[addresses[i]] = static_cast<long>(i % owners);
  std::ofstream out(dir / (name + ".expected"));
  for (const auto p : probes) {
    const auto it = expected.find(p);
    out << p << " " << (it == expected.end() ? -1L : it->second) << "\n";
  }
}

std::string read_all(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// SEG-008-T009: the chunked table through the real TranslationUnitSharder. Emits `<dir>/<name>/` (header, main TU, owner
// TUs, entry-chunk TUs, `probe.c` that includes the main TU and prints every lookup) plus `<name>.expected`, and a second
// emission `<name>_again/` that must be byte-identical.
void emit_chunked(const std::filesystem::path &directory, const std::vector<std::uint32_t> &addresses, std::size_t owners,
                  std::size_t chunk_entries) {
  const std::size_t chunks = compiled_entry_chunk_count(addresses.size(), chunk_entries);
  TranslationUnitSharder sharder(directory, "ct", {TranslationUnitFamily{"owner", 3, 0}, TranslationUnitFamily{"entry", chunks, 0}});
  std::ostream &out = sharder.stream();
  shard_begin_header(out);
  out << "#include <stdint.h>\n#include <stddef.h>\ntypedef int (*Entry)(void);\n";
  shard_end_header(out);
  const std::size_t distinct = std::min(owners, addresses.size());
  for (std::size_t i = 0; i < distinct; ++i) {
    ShardUnitScope unit(out, "owner", i, "int owner_" + std::to_string(i) + "(void)");
    out << "int owner_" << i << "(void) { return " << i << "; }\n";
  }
  std::vector<CompiledEntryBinding> bindings;
  for (std::size_t i = 0; i < addresses.size(); ++i) bindings.push_back({addresses[i], "owner_" + std::to_string(i % owners)});
  CompiledEntryTableNames names;
  names.entry_type = "Entry";
  names.lookup = "entry_lookup";
  CompiledEntryChunking chunking;
  chunking.chunk_entries = chunk_entries;
  chunking.declare_owner = [](std::string_view symbol) { return "int " + std::string(symbol) + "(void)"; };
  check(compiled_entry_table_is_chunked(out, addresses.size(), chunking) == (addresses.size() > chunk_entries), "chunk decision");
  check(emit_compiled_entry_table_chunked(out, bindings, names, chunking).empty(), "emit chunked");
  check(sharder.finish().empty(), "finish chunked");
}

void check_chunked(const std::filesystem::path &dir, const std::string &name, const std::vector<std::uint32_t> &addresses,
                   std::size_t owners, std::size_t chunk_entries) {
  const auto first = dir / name;
  const auto again = dir / (name + "_again");
  std::filesystem::remove_all(first);  // a previous run leaves compiled objects behind
  std::filesystem::remove_all(again);
  std::filesystem::create_directories(first);
  std::filesystem::create_directories(again);
  emit_chunked(first, addresses, owners, chunk_entries);
  emit_chunked(again, addresses, owners, chunk_entries);
  std::size_t files = 0;
  for (const auto &entry : std::filesystem::directory_iterator(first)) {
    ++files;
    check(read_all(entry.path()) == read_all(again / entry.path().filename()), "chunked output is not byte-identical");
  }
  check(files >= 3U + compiled_entry_chunk_count(addresses.size(), chunk_entries), "chunk TUs missing");
  std::set<std::uint32_t> probes = {0U, 1U, 0xFFFFFFFFU, 0xFFFFFFFEU, 0x00FFFFFFU, 0x01000000U};
  for (const auto a : addresses) { probes.insert(a); probes.insert(a + 1U); probes.insert(a - 1U); }
  std::ofstream c(first / "probe.c");
  c << "#include \"ct_main.c\"\n#include <stdio.h>\nint main(void) {\n  static const uint32_t probes[] = {\n";
  for (const auto p : probes) c << "    UINT32_C(" << p << "),\n";
  c << "  };\n  for (size_t i = 0; i < sizeof probes / sizeof probes[0]; ++i) {\n"
    << "    Entry e = entry_lookup(probes[i]);\n    printf(\"%u %d\\n\", (unsigned)probes[i], e == NULL ? -1 : e());\n  }\n  return 0;\n}\n";
  std::map<std::uint32_t, long> expected;
  for (std::size_t i = 0; i < addresses.size(); ++i) expected[addresses[i]] = static_cast<long>(i % owners);
  std::ofstream out(first / "probe.expected");
  for (const auto p : probes) {
    const auto it = expected.find(p);
    out << p << " " << (it == expected.end() ? -1L : it->second) << "\n";
  }
}
}  // namespace

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  const std::filesystem::path dir = argv[1];
  std::filesystem::create_directories(dir);
  check_widths();
  {
    std::ostringstream unsorted;
    check(!emit_compiled_entry_table(unsorted, {{4U, "a"}, {4U, "a"}}).empty(), "duplicate address rejected");
    check(!emit_compiled_entry_table(unsorted, {{5U, "a"}, {4U, "a"}}).empty(), "descending address rejected");
  }
  {
    // SEG-036-T002: the direct-entry table fails closed on malformed bindings.
    std::ostringstream header, unit;
    check(!emit_compiled_entry_table_direct(header, unit, {}).empty(), "direct: empty rejected");
    check(!emit_compiled_entry_table_direct(header, unit, {{5U, "a", ""}, {4U, "", "h", "0ULL"}}).empty(), "direct: descending rejected");
    check(!emit_compiled_entry_table_direct(header, unit, {{4U, "a", "h", "0ULL"}}).empty(), "direct: owner and helper together rejected");
    check(!emit_compiled_entry_table_direct(header, unit, {{4U, "", "", ""}}).empty(), "direct: neither owner nor helper rejected");
    check(!emit_compiled_entry_table_direct(header, unit, {{4U, "", "h", ""}}).empty(), "direct: missing provenance word rejected");
    std::ostringstream all_header, all_direct;
    check(emit_compiled_entry_table_direct(all_header, all_direct,
                                           {{4U, "", "h0", "0x0000000400000204ULL"}, {6U, "", "h1", "0ULL"}}).empty(),
          "direct: all-direct table accepted");
    check(all_direct.str().find("  NULL,\n") != std::string::npos, "direct: placeholder owner when every entry is direct");
    check(all_direct.str().find("  1,\n  2,\n") != std::string::npos, "direct: helper ids start after the owner count");
    check(all_header.str().find("static inline size_t genesis_compiled_entry_find") != std::string::npos,
          "direct: inline find in the header");
  }
  check_lookup(dir, "empty_like", {0U}, 1U, 8U);
  check_lookup(dir, "small", {2U, 4U, 6U, 0x200U, 0x00FFFFFEU, 0xFFFFFFFFU}, 2U, 8U);
  std::vector<std::uint32_t> many;
  for (std::uint32_t i = 0; i < 600U; ++i) many.push_back(0x100U + i * 2U);
  check_lookup(dir, "w8_max", many, 256U, 8U);
  check_lookup(dir, "w16_min", many, 257U, 16U);
  // Chunked (SEG-008-T009): chunk sizes 1 (one entry per TU), an exact divisor (600 = 6 x 100) and a non-divisor with a short tail
  // chunk; owners shared across chunks.
  check_chunked(dir, "chunk_1", {2U, 4U, 6U, 0xFFFFFFFFU}, 2U, 1U);
  check_chunked(dir, "chunk_100", many, 7U, 100U);
  check_chunked(dir, "chunk_499", many, 257U, 499U);
  std::ostringstream none;
  check(emit_compiled_entry_table(none, {}).empty() && none.str().find("return NULL") != std::string::npos, "empty table");
  return failures == 0 ? 0 : 1;
}
