// SEG-022-T009: compact compiled-entry table: width selection at count boundaries, and exact lookup
// equivalence (every valid, boundary, neighbouring and out-of-range address) of the emitted C against a
// reference std::map, compiled as strict C11.
#include "segarecomp/codegen/c11/compiled_entry_table.hpp"

#include <cstdlib>
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
void check_lookup(const std::filesystem::path &dir, const std::string &cc, const std::string &name,
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
  const auto source = dir / (name + ".c"), binary = dir / name, output = dir / (name + ".out");
  std::ofstream(source) << c.str();
  const std::string command = cc + " -std=c11 -Wall -Wextra -Werror -pedantic -o " + binary.string() + " " + source.string();
  check(std::system(command.c_str()) == 0, "strict C11 compile");
  check(std::system((binary.string() + " > " + output.string()).c_str()) == 0, "run");
  std::ifstream in(output);
  std::map<std::uint32_t, long> expected;
  for (std::size_t i = 0; i < addresses.size(); ++i) expected[addresses[i]] = static_cast<long>(i % owners);
  unsigned long address = 0;
  long got = 0;
  std::size_t seen = 0;
  while (in >> address >> got) {
    const auto it = expected.find(static_cast<std::uint32_t>(address));
    check(got == (it == expected.end() ? -1 : it->second), "lookup equivalence");
    ++seen;
  }
  check(seen == probes.size(), "all probes answered");
}
}  // namespace

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  const std::string cc = argv[1];
  const std::filesystem::path dir = argv[2];
  std::filesystem::create_directories(dir);
  check_widths();
  {
    std::ostringstream unsorted;
    check(!emit_compiled_entry_table(unsorted, {{4U, "a"}, {4U, "a"}}).empty(), "duplicate address rejected");
    check(!emit_compiled_entry_table(unsorted, {{5U, "a"}, {4U, "a"}}).empty(), "descending address rejected");
  }
  check_lookup(dir, cc, "empty_like", {0U}, 1U, 8U);
  check_lookup(dir, cc, "small", {2U, 4U, 6U, 0x200U, 0x00FFFFFEU, 0xFFFFFFFFU}, 2U, 8U);
  std::vector<std::uint32_t> many;
  for (std::uint32_t i = 0; i < 600U; ++i) many.push_back(0x100U + i * 2U);
  check_lookup(dir, cc, "w8_max", many, 256U, 8U);
  check_lookup(dir, cc, "w16_min", many, 257U, 16U);
  std::ostringstream none;
  check(emit_compiled_entry_table(none, {}).empty() && none.str().find("return NULL") != std::string::npos, "empty table");
  return failures == 0 ? 0 : 1;
}
