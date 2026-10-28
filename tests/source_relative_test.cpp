// SEG-096-T002 (ADR 0207): the text-level derivation of a source-relative body from three lowerings of one instruction.
#include <cstdio>
#include <string>

#include "segarecomp/codegen/c11/source_relative.hpp"

namespace {
int failures = 0;
void check(bool ok, const char *label) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", label);
  if (!ok) ++failures;
}
using segarecomp::SourceRelativeStatus;
constexpr std::array<std::uint32_t, 3> kPcs{0x00200010U, 0x00300010U, 0x00480010U};

// The three lowerings are produced by printing the formula the CPU lowering would evaluate at each PC.
std::array<std::string, 3> at(const char *format_head, std::uint32_t delta, const char *tail = ");") {
  std::array<std::string, 3> out;
  for (std::size_t i = 0; i < 3; ++i) {
    char buffer[128];
    std::snprintf(buffer, sizeof buffer, "%sUINT32_C(0x%08X%s", format_head, kPcs[i] + delta, tail);
    out[i] = buffer;
  }
  return out;
}
}  // namespace

int main() {
  using namespace segarecomp;
  {  // a branch target: PC + 0x22
    const auto r = derive_source_relative_text(at("pc = ", 0x22U), kPcs, "scd_pc0");
    check(r.status == SourceRelativeStatus::ok && r.relative_literals == 1U && r.text == "pc = (uint32_t)(scd_pc0 + UINT32_C(0x00000022));",
          "a PC-derived literal becomes pc0 + constant (the UINT32_C call is replaced as a whole)");
  }
  {  // a negative displacement wraps modulo 2^32
    const auto r = derive_source_relative_text(at("pc = ", 0xFFFFFF00U), kPcs, "scd_pc0");
    check(r.status == SourceRelativeStatus::ok && r.text.find("UINT32_C(0xFFFFFF00)") != std::string::npos, "a negative displacement keeps its modulo-2^32 delta");
  }
  {  // an absolute address does not move: untouched
    const std::array<std::string, 3> same{"x = UINT32_C(0x00FF8005);", "x = UINT32_C(0x00FF8005);", "x = UINT32_C(0x00FF8005);"};
    const auto r = derive_source_relative_text(same, kPcs, "scd_pc0");
    check(r.status == SourceRelativeStatus::ok && r.relative_literals == 0U && r.text == same[0], "an absolute literal is left exactly as the lowering wrote it");
  }
  {  // mixed: one absolute, one relative
    std::array<std::string, 3> mixed;
    for (std::size_t i = 0; i < 3; ++i) {
      char buffer[160];
      std::snprintf(buffer, sizeof buffer, "w(UINT32_C(0x00FF8005), UINT32_C(0x%08X), 4U);", kPcs[i] + 6U);
      mixed[i] = buffer;
    }
    const auto r = derive_source_relative_text(mixed, kPcs, "scd_pc0");
    check(r.status == SourceRelativeStatus::ok && r.relative_literals == 1U && r.text.find("UINT32_C(0x00FF8005)") != std::string::npos &&
              r.text.find("scd_pc0 + UINT32_C(0x00000006)") != std::string::npos && r.text.find("4U") != std::string::npos,
          "absolute and PC-derived literals in one statement are told apart");
  }
  {  // a literal that is masked (does not move linearly)
    std::array<std::string, 3> masked;
    for (std::size_t i = 0; i < 3; ++i) {
      char buffer[64];
      std::snprintf(buffer, sizeof buffer, "pc = UINT32_C(0x%08X);", (kPcs[i] + 2U) & 0x000FFFFFU);
      masked[i] = buffer;
    }
    const auto r = derive_source_relative_text(masked, kPcs, "scd_pc0");
    check(r.status == SourceRelativeStatus::literal_not_linear && r.text.empty(), "a masked (non-linear) literal is refused, not guessed");
  }
  {  // an identifier that embeds the PC
    const std::array<std::string, 3> named{"m68k_indirect_targets_00200010[0];", "m68k_indirect_targets_00300010[0];", "m68k_indirect_targets_00480010[0];"};
    check(derive_source_relative_text(named, kPcs, "scd_pc0").status == SourceRelativeStatus::token_structure, "an identifier embedding the PC is refused");
  }
  {  // different structure
    const std::array<std::string, 3> shape{"a = 1;", "a = 1; b = 2;", "a = 1;"};
    check(derive_source_relative_text(shape, kPcs, "scd_pc0").status == SourceRelativeStatus::token_structure, "texts of different shape are refused");
  }
  {  // a case label cannot take a runtime expression
    const auto r = derive_source_relative_text(at("switch (x) { case ", 0x4U, "): break; }"), kPcs, "scd_pc0");
    check(r.status == SourceRelativeStatus::constant_context, "a PC-derived case label is refused (constant expression required)");
  }
  {  // a suffix change
    const std::array<std::string, 3> suffix{"x = 0x200012U;", "x = 0x300012UL;", "x = 0x480012U;"};
    check(derive_source_relative_text(suffix, kPcs, "scd_pc0").status == SourceRelativeStatus::literal_suffix, "a changing literal suffix is refused");
  }
  {  // equal literals that happen to look like the PC but do not move
    const std::array<std::string, 3> frozen{"x = 0x200012U;", "x = 0x200012U;", "x = 0x200012U;"};
    check(derive_source_relative_text(frozen, kPcs, "scd_pc0").relative_literals == 0U, "a literal equal in all lowerings is never treated as PC-derived");
  }
  std::printf("%d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
