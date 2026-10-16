// SEG-063-T001 Checkpoint A (ADR 0126, 0127, 0132): the Sega CD compilation boundary for several images per processor instance.
//
// Synthetic image sets (hand-encoded MC68000 words, no disc) prove that:
//   * two different byte sequences at one PC are compiled into two distinct functions and are NOT reachable through the per-PC
//     dispatch switch: only the platform overlay selector (scd_overlay_dispatch) reaches them, and only through the candidate's own
//     dispatcher;
//   * the same bytes mapped at two bases are two compilation units with their own absolute PCs;
//   * two processor instances with overlapping numerical PCs get disjoint registries (symbols, groups) - processor identity is
//     platform-owned, both sets stay CpuVariant::mc68000;
//   * invalid mappings are rejected (cross-image overlap without the platform selector, intra-image overlap, roles not parallel);
//   * structural validation is not byte identity: the validator accepts images whose bytes differ (A vs A') or coincide, and it is
//     the live-byte comparison at run time that distinguishes them;
//   * a set without modules emits exactly the SEG-062 text (no overlay table, the SEG-062 default stop);
//   * generated text contains no decoder or interpreter.
#include <cstdint>
#include <cstdio>
#include <regex>
#include <string>
#include <vector>

#include "segarecomp/machine/sega_cd/program.hpp"

namespace {

using namespace segarecomp;
using namespace segarecomp::machine::sega_cd;

int failures = 0;

void check(bool ok, const std::string &label) {
  std::printf("%-5s %s\n", ok ? "ok" : "FAIL", label.c_str());
  if (!ok) ++failures;
}

std::vector<std::uint8_t> words(std::initializer_list<std::uint16_t> list) {
  std::vector<std::uint8_t> out;
  for (const auto w : list) {
    out.push_back(static_cast<std::uint8_t>(w >> 8));
    out.push_back(static_cast<std::uint8_t>(w));
  }
  return out;
}

// moveq #1,d0 ; move.w #$AAAA,$00010020 ; rts
const auto kModuleA = words({0x7001, 0x33FC, 0xAAAA, 0x0001, 0x0020, 0x4E75});
// moveq #1,d0 ; moveq #2,d1 ; move.w #$BBBB,$00010022 ; rts   (first instruction identical to A's, then different)
const auto kModuleB = words({0x7001, 0x7202, 0x33FC, 0xBBBB, 0x0001, 0x0022, 0x4E75});
// stop #$2700
const auto kBoot = words({0x4E72, 0x2700});

ExecutableImage image(std::uint32_t id, std::vector<std::uint8_t> bytes, std::vector<ImageMapping> mappings, const char *producer) {
  ExecutableImage result;
  result.id = ImageId{id};
  result.bytes = std::move(bytes);
  result.mappings = std::move(mappings);
  result.provenance.authority = ImageAuthority::immutable_input;
  result.provenance.producer = producer;
  result.verification = ImageVerification::byte_identity;
  return result;
}

ProcessorImages boot(Processor processor, std::uint32_t base) {
  ProcessorImages p;
  p.processor = processor;
  p.images.cpu = CpuVariant::mc68000;
  p.images.images.push_back(image(1, kBoot, {{base, 0, static_cast<std::uint32_t>(kBoot.size())}},
                                  processor == Processor::main ? "sega_cd.boot_ip" : "sega_cd.boot_sp"));
  p.entry_pc = base;
  p.roles.push_back({processor == Processor::main ? "boot_ip" : "boot_sp", false, {0}});
  return p;
}

void add_module(ProcessorImages &p, const std::string &name, const std::vector<std::uint8_t> &bytes, std::vector<std::uint32_t> bases) {
  std::vector<ImageMapping> mappings;
  for (const auto base : bases) mappings.push_back({base, 0, static_cast<std::uint32_t>(bytes.size())});
  p.images.platform_selector = true;
  p.images.images.push_back(image(static_cast<std::uint32_t>(p.images.images.size() + 1U), bytes, std::move(mappings),
                                  "sega_cd.disc_module"));
  p.roles.push_back({name, true, {0}});
}

BootImages program() {
  BootImages images;
  images.main = boot(Processor::main, 0x00FF0000U);
  images.sub = boot(Processor::sub, 0x00006000U);
  return images;
}

bool has(const std::string &text, const std::string &needle) { return text.find(needle) != std::string::npos; }

std::string between(const std::string &text, const std::string &from, const std::string &to) {
  const auto a = text.find(from);
  if (a == std::string::npos) return {};
  const auto b = text.find(to, a);
  return text.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

}  // namespace

int main() {
  // --- baseline: no modules -> SEG-062 shape -----------------------------------------------------------------------------------
  {
    const auto e = emit_sega_cd_processors(program());
    check(e.error == EmissionError::none && e.overlay_groups == 0U && e.overlay_source.empty(), "no modules: no overlay registry");
    check(has(e.c_source, "default: return scd_fail(m, 1U, SCD_STOP_ENTRY_NOT_COMPILED") &&
              !has(e.c_source, "scd_overlay_dispatch"),
          "no modules: the SEG-062 default stop, no overlay selector");
    check(has(e.c_source, "static int scd_p1_i_00006000(ScdMachine *m)"), "no modules: SEG-062 symbol names");
  }

  // --- two different byte sequences at one PC ----------------------------------------------------------------------------------
  {
    auto images = program();
    add_module(images.sub, "module_a", kModuleA, {0x00020000U});
    add_module(images.sub, "module_b", kModuleB, {0x00020000U});
    const auto e = emit_sega_cd_processors(images);
    check(e.error == EmissionError::none && e.overlay_groups == 1U, "A and B at $020000: one overlay group");
    check(has(e.c_source, "static int scd_p1_m2_i_00020000(ScdMachine *m)") && has(e.c_source, "static int scd_p1_m3_i_00020000(ScdMachine *m)"),
          "A and B each have their own compiled function at the shared PC $020000");
    check(has(e.c_source, "static int scd_p1_m2_i_00020002(") && has(e.c_source, "static int scd_p1_m2_i_0002000A(") &&
              has(e.c_source, "static int scd_p1_m3_i_00020002(") && has(e.c_source, "static int scd_p1_m3_i_00020004(") && has(e.c_source, "static int scd_p1_m3_i_0002000C("),
          "A's and B's later instructions are compiled at their own boundaries");
    check(!has(e.c_source, "static int scd_p1_m2_i_00020004") && has(e.c_source, "static int scd_p1_m3_i_00020004"),
          "an instruction boundary of B ($020004) is not invented for A (A's second instruction spans it)");
    const auto dispatch = between(e.c_source, "static int scd_p1_dispatch(", "\n}\n");
    check(!dispatch.empty() && !has(dispatch, "0x00020000") && has(dispatch, "default: return scd_overlay_dispatch(m, 1U);"),
          "the per-PC dispatch switch never names an overlay PC; its default is the platform selector");
    check(has(dispatch, "case UINT32_C(0x00006000): return scd_p1_i_00006000(m);"), "the boot SP keeps its SEG-062 per-PC case");
    const auto entries = between(e.c_source, "static const uint32_t scd_p1_entries[] = {", "};");
    check(entries.find("0x00020000") != std::string::npos && entries.find("0x00020000") == entries.rfind("0x00020000"),
          "the computed-control entry table holds the shared PC once (it authorises an address, never an image)");
    const auto a_dispatch = between(e.overlay_source, "static int scd_p1_m2_c_dispatch(", "\n}\n");
    const auto b_dispatch = between(e.overlay_source, "static int scd_p1_m3_c_dispatch(", "\n}\n");
    check(has(a_dispatch, "return scd_p1_m2_i_00020000(m);") && !has(a_dispatch, "scd_p1_m3_") &&
              has(b_dispatch, "return scd_p1_m3_i_00020000(m);") && !has(b_dispatch, "scd_p1_m2_") &&
              has(a_dispatch, "default: return SCD_IMAGE_NO_ENTRY;"),
          "each candidate's dispatcher reaches only its own image's entries");
    check(has(e.overlay_source, "{2U, \"module_a\", UINT32_C(0x00020000), UINT32_C(0x0000000C), scd_p1_m2_c_digests, 2048U, 1U") &&
              has(e.overlay_source, "{3U, \"module_b\", UINT32_C(0x00020000), UINT32_C(0x0000000E), scd_p1_m3_c_digests, 2048U, 1U") &&
              has(e.overlay_source, "{1U, UINT32_C(0x00020000), UINT32_C(0x0002000E), scd_p1_g0_candidates, 2U, 2048U, 1U}"),
          "the registry lists both candidates in build order with their ranges and granules, in one group");
    const auto a_digest = between(e.overlay_source, "scd_p1_m2_c_digests[] = {", "};");
    const auto b_digest = between(e.overlay_source, "scd_p1_m3_c_digests[] = {", "};");
    check(!a_digest.empty() && a_digest.substr(a_digest.find('{')) != b_digest.substr(b_digest.find('{')),
          "granule digests distinguish A from B");
    check(e.images.size() == 4U && e.images[2].name == "module_a" && e.images[2].overlay_group == 0 && e.images[0].overlay_group == -1,
          "per-image statistics: boot images outside any group, modules in group 0");
  }

  // --- the same bytes at two bases ---------------------------------------------------------------------------------------------
  {
    auto images = program();
    add_module(images.sub, "module_a", kModuleA, {0x00080000U, 0x00020000U});
    const auto e = emit_sega_cd_processors(images);
    check(e.error == EmissionError::none && e.overlay_groups == 2U, "one image, two mappings: two candidates in two groups");
    check(has(e.c_source, "scd_p1_m2_map0_i_00080000") && has(e.c_source, "scd_p1_m2_map1_i_00020000"),
          "the same bytes are compiled at each base with their own absolute PCs");
    check(has(e.c_source, "UINT32_C(0x00080000)") && has(e.c_source, "UINT32_C(0x00020000)"), "both bases are compiled entries");
  }

  // --- two processor instances with overlapping numerical PCs -----------------------------------------------------------------
  {
    auto images = program();
    add_module(images.main, "main_module", kModuleA, {0x00020000U});  // main: the PRG-RAM window
    add_module(images.sub, "sub_module", kModuleB, {0x00020000U});    // sub: PRG RAM $020000 (other physical bytes)
    const auto e = emit_sega_cd_processors(images);
    check(e.error == EmissionError::none && e.overlay_groups == 2U, "main and sub modules at numerical PC $020000: two groups");
    check(has(e.c_source, "static int scd_p0_m2_i_00020000(ScdMachine *m)") && has(e.c_source, "static int scd_p1_m2_i_00020000(ScdMachine *m)"),
          "each processor instance has its own compiled entry for $020000");
    check(has(e.overlay_source, "{0U, UINT32_C(0x00020000)") && has(e.overlay_source, "{1U, UINT32_C(0x00020000)"),
          "groups carry the owning processor instance");
    check(has(between(e.overlay_source, "scd_p0_m2_c_dispatch(", "\n}\n"), "m->cpu[0].pc") &&
              has(between(e.overlay_source, "scd_p1_m2_c_dispatch(", "\n}\n"), "m->cpu[1].pc"),
          "each candidate dispatcher reads only its own processor's PC");
    check(images.main.images.cpu == CpuVariant::mc68000 && images.sub.images.cpu == CpuVariant::mc68000,
          "both sets stay CpuVariant::mc68000");
  }

  // --- invalid mappings ----------------------------------------------------------------------------------------------------------
  {
    auto images = program();
    add_module(images.sub, "module_a", kModuleA, {0x00020000U});
    add_module(images.sub, "module_b", kModuleB, {0x00020000U});
    images.sub.images.platform_selector = false;
    check(emit_sega_cd_processors(images).error == EmissionError::image_validation &&
              validate_executable_image_set(images.sub.images).error == ImageValidationError::overlapping_mappings,
          "cross-image overlap without the platform selector is rejected");
  }
  {
    auto images = program();
    add_module(images.sub, "module_a", kModuleA, {0x00020000U, 0x00020004U});
    check(validate_executable_image_set(images.sub.images).error == ImageValidationError::overlapping_mappings &&
              emit_sega_cd_processors(images).error == EmissionError::image_validation,
          "one image mapped twice over itself is rejected even with the selector");
  }
  {
    auto images = program();
    add_module(images.sub, "module_a", kModuleA, {0x00020000U});
    images.sub.roles.pop_back();
    check(emit_sega_cd_processors(images).error == EmissionError::roles_mismatch, "roles not parallel to the set are rejected");
  }
  {
    auto images = program();
    add_module(images.sub, "module_a", kModuleA, {0x00020000U});
    images.sub.roles.back().module = false;
    check(emit_sega_cd_processors(images).error == EmissionError::boot_image_shape, "a second non-module image is rejected");
  }

  // --- structural validation is not byte identity ---------------------------------------------------------------------------------
  {
    auto images = program();
    auto a_prime = kModuleA;
    a_prime[3] ^= 0x01U;  // A': one byte of the MOVE immediate differs
    add_module(images.sub, "module_a", kModuleA, {0x00020000U});
    add_module(images.sub, "module_a_prime", a_prime, {0x00020000U});
    add_module(images.sub, "module_a_copy", kModuleA, {0x00020000U});
    check(validate_executable_image_set(images.sub.images).ok(),
          "the structural validator accepts A, A' and a byte-identical copy at one PC (it never compares bytes)");
    const auto e = emit_sega_cd_processors(images);
    const auto d2 = between(e.overlay_source, "scd_p1_m2_c_digests[] = {", "};");
    const auto d3 = between(e.overlay_source, "scd_p1_m3_c_digests[] = {", "};");
    const auto d4 = between(e.overlay_source, "scd_p1_m4_c_digests[] = {", "};");
    check(e.error == EmissionError::none && d2.substr(d2.find('{')) != d3.substr(d3.find('{')) &&
              d2.substr(d2.find('{')) == d4.substr(d4.find('{')),
          "byte identity is carried separately: A' digests differ from A's, the copy's are equal (duplicates are a producer error)");
  }

  // --- registry bounds ----------------------------------------------------------------------------------------------------------
  {
    auto images = program();
    for (int i = 0; i < 17; ++i) add_module(images.sub, "m" + std::to_string(i), kModuleA, {0x00020000U});
    check(emit_sega_cd_processors(images).error == EmissionError::too_many_candidates, "more than 16 candidates in a group is refused");
  }
  {
    auto images = program();
    for (std::uint32_t i = 0; i < 17; ++i) add_module(images.sub, "m" + std::to_string(i), kModuleA, {0x00020000U + i * 0x100U});
    check(emit_sega_cd_processors(images).error == EmissionError::too_many_overlay_groups, "more than 16 overlay groups is refused");
  }

  // --- no decoder or interpreter ---------------------------------------------------------------------------------------------------
  {
    auto images = program();
    add_module(images.sub, "module_a", kModuleA, {0x00020000U});
    add_module(images.sub, "module_b", kModuleB, {0x00020000U});
    const auto e = emit_sega_cd_processors(images);
    const std::regex forbidden("decode|disassembl|interpret|jit|m68k_execute|opcode", std::regex::icase);
    check(!std::regex_search(e.c_source, forbidden) && !std::regex_search(e.overlay_source, forbidden),
          "generated text names no decoder, interpreter or JIT");
  }

  std::printf("%s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
  return failures == 0 ? 0 : 1;
}
