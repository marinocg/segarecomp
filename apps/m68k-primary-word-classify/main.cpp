// SEG-042-T001: the smallest practical production-side proof-path requalification mechanism.
//
// An external analysis producer (SEG-042-T002's automated harvester, building on
// `tools/segarecomp_angr_m68k_facts.py`) must never credit a semantic-completeness ("exact"/"contained")
// fact derived from a proof path that fetched an operation word this project's OWN generation-time
// authority (`m68k_classify_primary_word`, ADR 0043 section 3) does not itself recognize as a real
// base-MC68000 instruction -- SEG-041-T002 found concrete, reproducible over-acceptance on every
// available angr p-code 68000-family variant (68010+/68020+-only forms decoded as legal; Coldfire/
// 68020/68030-only reserved-space repurposing). The existing RTE-specific check in the Python producer
// catches one named semantic gap (RTE is legal-but-unsupported); it does not catch general over-
// acceptance of an otherwise-illegal/reserved word by the backend's p-code variant.
//
// This is a tiny, bounded, non-production (never linked into `segarecomp`/the compiler/runtime) batch
// query CLI over the exact same pure function the decoder itself uses at generation time -- not a
// reimplementation, not the independent test-side legal-form dataset (that dataset remains decoupled
// from every consumer except its own two already-whitelisted ones; this tool reads neither it nor any
// oracle). No decoding logic is duplicated in Python: the Python caller only ever sees this process's
// stdout classification, one line per distinct word it asks about.
//
// Usage: segarecomp-m68k-primary-word-classify --words <hex4>[,<hex4>...]
// stdout: one line per word, in the order given: "<hex4> <class>", class one of
//   legal | line_a_emulator | line_f_emulator | illegal
// Exit 0 on success; exit 2 on a malformed/empty/oversized argument (usage error, nothing printed).
#include "segarecomp/cpu/m68k/decode.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Bounded like every other untrusted/batch input this project accepts; the whole primary-word space is
// 65536 words, so this ceiling can never legitimately be exceeded by a sound caller.
constexpr std::size_t max_words = 65536U;

const char *class_name(segarecomp::M68kPrimaryWordClass word_class) noexcept {
  switch (word_class) {
  case segarecomp::M68kPrimaryWordClass::legal: return "legal";
  case segarecomp::M68kPrimaryWordClass::line_a_emulator: return "line_a_emulator";
  case segarecomp::M68kPrimaryWordClass::line_f_emulator: return "line_f_emulator";
  case segarecomp::M68kPrimaryWordClass::illegal: return "illegal";
  }
  return "illegal";
}

void usage(std::ostream &out) { out << "usage: segarecomp-m68k-primary-word-classify --words <hex4>[,<hex4>...]\n"; }

}  // namespace

int main(int argc, char **argv) {
  if (argc != 3 || std::string_view(argv[1]) != "--words") {
    usage(std::cerr);
    return 2;
  }
  std::string_view text(argv[2]);
  std::vector<std::uint16_t> words;
  while (!text.empty()) {
    const auto comma = text.find(',');
    const auto entry = text.substr(0, comma);
    if (entry.size() != 4U) {
      usage(std::cerr);
      return 2;
    }
    std::uint32_t value{};
    for (const char c : entry) {
      const bool digit = c >= '0' && c <= '9', lower = c >= 'a' && c <= 'f', upper = c >= 'A' && c <= 'F';
      if (!digit && !lower && !upper) {
        usage(std::cerr);
        return 2;
      }
      value = (value << 4U) | static_cast<std::uint32_t>(digit ? c - '0' : lower ? c - 'a' + 10 : c - 'A' + 10);
    }
    words.push_back(static_cast<std::uint16_t>(value));
    if (words.size() > max_words) {
      usage(std::cerr);
      return 2;
    }
    if (comma == std::string_view::npos) break;
    text.remove_prefix(comma + 1U);
    if (text.empty()) {
      usage(std::cerr);
      return 2;
    }
  }
  if (words.empty()) {
    usage(std::cerr);
    return 2;
  }
  std::array<char, 4> hex{};
  for (const std::uint16_t word : words) {
    for (int i = 0; i < 4; ++i) {
      const unsigned nibble = (static_cast<unsigned>(word) >> (12U - 4U * static_cast<unsigned>(i))) & 0xFU;
      hex[static_cast<std::size_t>(i)] = static_cast<char>(nibble < 10U ? '0' + nibble : 'a' + nibble - 10U);
    }
    std::cout << hex[0] << hex[1] << hex[2] << hex[3] << ' ' << class_name(segarecomp::m68k_classify_primary_word(word)) << '\n';
  }
  return 0;
}
