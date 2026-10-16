// SEG-062-T001 (ADR 0126): model-level proof that the unchanged CPU-neutral ExecutableImageSet expresses every Sega CD executable
// case with one set per processor instance (both CpuVariant::mc68000), and that the generic structural validator accepts and
// rejects exactly what ADR 0126 says. No production code: the processor-instance enumerator below is a test-local stand-in for the
// platform-owned identity (platforms/sega-cd). Disc extents appear only as producer evidence facts.
//
// tests/sega_cd_executable_image_mutation_test.py recompiles this file against a mutated validator (cross-image overlap allowed
// without platform_selector) and requires it to FAIL.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "segarecomp/recompiler/executable_image.hpp"

namespace {

using namespace segarecomp;

int failures = 0;

void check(bool ok, const std::string &label) {
  std::printf("%-5s %s\n", ok ? "ok" : "FAIL", label.c_str());
  if (!ok) ++failures;
}

enum class Processor { main, sub };  // platform identity, never part of the generic set

std::vector<std::uint8_t> code(std::size_t size, unsigned seed) {
  std::vector<std::uint8_t> bytes(size);
  for (std::size_t i = 0; i < size; ++i) bytes[i] = static_cast<std::uint8_t>((i * 29U + seed * 7U + 1U) & 0xFFU);
  return bytes;
}

ExecutableImage image(std::uint32_t id, std::vector<std::uint8_t> bytes, std::vector<ImageMapping> mappings, ImageAuthority authority,
                      const std::string &producer, ImageVerification verification, std::vector<ImageEvidenceFact> evidence = {}) {
  ExecutableImage result;
  result.id = ImageId{id};
  result.bytes = std::move(bytes);
  result.mappings = std::move(mappings);
  result.provenance.authority = authority;
  result.provenance.producer = producer;
  result.provenance.evidence = std::move(evidence);
  result.verification = verification;
  return result;
}

struct InstanceSet {
  Processor processor;
  ExecutableImageSet set;
};

ImageValidationError error_of(const ExecutableImageSet &set) { return validate_executable_image_set(set).error; }

}  // namespace

int main() {
  // Case 1: IP/SP boot images, one set per processor instance, both the same ISA variant.
  InstanceSet main_set{Processor::main, {CpuVariant::mc68000, false,
                                         {image(1, code(0x600, 1), {{0x00FF0000U, 0U, 0x600U}}, ImageAuthority::immutable_input,
                                                "sega_cd.boot_ip", ImageVerification::byte_identity,
                                                {{"disc.extent", "track:1 byte:512 length:1536"}})}}};
  InstanceSet sub_set{Processor::sub, {CpuVariant::mc68000, false,
                                       {image(1, code(0x7800, 2), {{0x00006000U, 0U, 0x7800U}}, ImageAuthority::immutable_input,
                                              "sega_cd.boot_sp", ImageVerification::byte_identity,
                                              {{"disc.extent", "track:1 byte:2048 length:30720"}})}}};
  check(main_set.set.cpu == sub_set.set.cpu && main_set.set.cpu == CpuVariant::mc68000,
        "case 1: both processor instances carry CpuVariant::mc68000 (no new ISA tag)");
  check(main_set.processor != sub_set.processor, "case 1: processor identity is platform-owned and distinct");
  check(validate_executable_image_set(main_set.set).ok() && validate_executable_image_set(sub_set.set).ok(),
        "case 1: IP (main) and SP (sub) boot sets validate");
  // The two sets are independent: the same execution address in both is not an overlap of either set.
  InstanceSet sub_same_address = sub_set;
  sub_same_address.set.images[0].mappings[0].execution_base = 0x00FF0000U;
  check(validate_executable_image_set(sub_same_address.set).ok(), "case 1: sets of different instances never conflict");

  // Case 2: a disc-loaded module; the extent is evidence only.
  ExecutableImageSet module_set{CpuVariant::mc68000, false,
                                {image(1, code(0x2000, 3), {{0x00010000U, 0U, 0x2000U}}, ImageAuthority::immutable_input,
                                       "sega_cd.disc_module", ImageVerification::byte_identity,
                                       {{"disc.extent", "lba:40 sectors:4"}})}};
  check(validate_executable_image_set(module_set).ok(), "case 2: disc-loaded module validates");
  ExecutableImageSet bad_producer = module_set;
  bad_producer.images[0].provenance.producer = "Sega-CD Module";
  check(error_of(bad_producer) == ImageValidationError::invalid_producer_name, "case 2: producer name outside [a-z0-9_.] rejected");

  // Case 3: one module at several addresses (same bytes, two mappings); overlap inside one image is always rejected.
  ExecutableImageSet multi_base{CpuVariant::mc68000, false,
                                {image(1, code(0x1000, 4), {{0x00010000U, 0U, 0x1000U}, {0x00020000U, 0U, 0x1000U}},
                                       ImageAuthority::immutable_input, "sega_cd.disc_module", ImageVerification::byte_identity)}};
  check(validate_executable_image_set(multi_base).ok(), "case 3: one module mapped at two bases validates");
  ExecutableImageSet self_overlap = multi_base;
  self_overlap.images[0].mappings[1].execution_base = 0x00010800U;
  check(error_of(self_overlap) == ImageValidationError::overlapping_mappings, "case 3: intra-image overlap rejected");
  self_overlap.platform_selector = true;
  check(error_of(self_overlap) == ImageValidationError::overlapping_mappings,
        "case 3: intra-image overlap rejected even with platform_selector");

  // Case 4: several modules at one PC: only with a platform selector.
  ExecutableImageSet shared_pc{CpuVariant::mc68000, false,
                               {image(1, code(0x1000, 5), {{0x00030000U, 0U, 0x1000U}}, ImageAuthority::immutable_input,
                                      "sega_cd.disc_module", ImageVerification::byte_identity),
                                image(2, code(0x0800, 6), {{0x00030000U, 0U, 0x0800U}}, ImageAuthority::immutable_input,
                                      "sega_cd.disc_module", ImageVerification::byte_identity)}};
  check(error_of(shared_pc) == ImageValidationError::overlapping_mappings,
        "case 4: two images at one PC rejected without platform_selector");
  shared_pc.platform_selector = true;
  check(validate_executable_image_set(shared_pc).ok(), "case 4: two images at one PC accepted with platform_selector");
  ExecutableImageSet adjacent = shared_pc;
  adjacent.platform_selector = false;
  adjacent.images[1].mappings[0].execution_base = 0x00031000U;
  check(validate_executable_image_set(adjacent).ok(), "case 4: adjacent (non-overlapping) images need no selector");

  // Case 5: static-proof copy referencing an earlier image's bytes (no copy of bytes).
  ExecutableImageSet copy_set{CpuVariant::mc68000, false,
                              {image(1, code(0x1000, 7), {{0x00040000U, 0U, 0x1000U}}, ImageAuthority::immutable_input,
                                     "sega_cd.disc_module", ImageVerification::byte_identity)}};
  ExecutableImage alias;
  alias.id = ImageId{2};
  alias.source = ImageSourceReference{ImageId{1}, 0x100U, 0x200U};
  alias.mappings = {{0x00200000U, 0U, 0x200U}};
  alias.provenance = {ImageAuthority::static_proof, "sega_cd.static_copy", {}};
  alias.verification = ImageVerification::byte_identity;
  copy_set.images.push_back(alias);
  check(validate_executable_image_set(copy_set).ok(), "case 5: static-proof copy resolves its source reference");
  check(executable_image_bytes(copy_set, copy_set.images[1]).size() == 0x200U &&
            executable_image_bytes(copy_set, copy_set.images[1])[0] == copy_set.images[0].bytes[0x100],
        "case 5: alias bytes are the source's bytes at the referenced offset");
  ExecutableImageSet bad_alias = copy_set;
  bad_alias.images[1].source->source = ImageId{2};
  check(error_of(bad_alias) == ImageValidationError::unresolved_source, "case 5: self/forward source reference rejected");
  bad_alias = copy_set;
  bad_alias.images[1].source->length = 0x1000U;
  check(error_of(bad_alias) == ImageValidationError::source_out_of_range, "case 5: source range beyond the source rejected");

  // Case 6: bounded build-time materialized RAM code.
  ExecutableImageSet materialized{CpuVariant::mc68000, false,
                                  {image(1, code(0x80, 8), {{0x00FF8000U, 0U, 0x80U}},
                                         ImageAuthority::bounded_build_time_materialization, "sega_cd.materialized",
                                         ImageVerification::byte_identity)}};
  check(validate_executable_image_set(materialized).ok(), "case 6: bounded materialized RAM code validates");

  // Structural rejections the boot producer relies on.
  ExecutableImageSet empty_mapping = main_set.set;
  empty_mapping.images[0].mappings[0].length = 0U;
  check(error_of(empty_mapping) == ImageValidationError::empty_mapping, "boot: empty mapping rejected");
  ExecutableImageSet outside = main_set.set;
  outside.images[0].mappings[0].length = 0x601U;
  check(error_of(outside) == ImageValidationError::mapping_outside_bytes, "boot: mapping beyond the image bytes rejected");
  ExecutableImageSet wraps = main_set.set;
  wraps.images[0].mappings[0].execution_base = 0xFFFFFC00U;
  check(error_of(wraps) == ImageValidationError::mapping_wraps, "boot: mapping leaving the 32-bit space rejected");
  ExecutableImageSet sparse = main_set.set;
  sparse.images[0].id = ImageId{2};
  check(error_of(sparse) == ImageValidationError::id_not_dense, "boot: non-dense image id rejected");

  // Provenance counts are sanitized (counts and producer names only).
  const auto json = format_image_provenance_json(count_image_provenance(copy_set));
  check(json.find("disc.extent") == std::string::npos && json.find("\"sega_cd.static_copy\":1") != std::string::npos,
        "provenance report carries producer counts, never evidence facts");

  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("sega_cd_executable_image_test: all checks passed\n");
  return 0;
}
