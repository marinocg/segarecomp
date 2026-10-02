// SEG-028-T002 (ADR 0077): the CPU-neutral executable-image artifact (libs/recompiler executable_image.hpp).
//
// Synthetic, project-authored fixtures only. Every ImageValidationError value is produced by a minimal mutant of a valid set and
// detected with the exact error and image index; valid shapes (empty set, owned bytes, source reference, several mappings, overlapping
// images under a platform selector) validate; byte resolution, the sanitized provenance counts and their JSON form are deterministic.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "segarecomp/recompiler/executable_image.hpp"

namespace {

using namespace segarecomp;

int failures = 0;

void check(bool ok, const std::string& label) {
  std::printf("%-5s %s\n", ok ? "ok" : "FAIL", label.c_str());
  if (!ok) ++failures;
}

std::vector<std::uint8_t> pattern(std::size_t size, unsigned seed) {
  std::vector<std::uint8_t> bytes(size);
  for (std::size_t i = 0; i < size; ++i) bytes[i] = static_cast<std::uint8_t>((i * 13U + seed * 29U + 5U) & 0xFFU);
  return bytes;
}

ExecutableImage owned(std::uint32_t id, std::size_t size, std::vector<ImageMapping> mappings, std::string producer = "test.owner",
                      ImageAuthority authority = ImageAuthority::immutable_input) {
  ExecutableImage image;
  image.id = ImageId{id};
  image.bytes = pattern(size, id);
  image.mappings = std::move(mappings);
  image.provenance.authority = authority;
  image.provenance.producer = std::move(producer);
  image.provenance.evidence.push_back({"derivation", "synthetic"});
  return image;
}

ExecutableImage borrowed(std::uint32_t id, ImageSourceReference ref, std::vector<ImageMapping> mappings) {
  ExecutableImage image;
  image.id = ImageId{id};
  image.source = ref;
  image.mappings = std::move(mappings);
  image.provenance.authority = ImageAuthority::static_proof;
  image.provenance.producer = "test.borrower";
  image.verification = ImageVerification::byte_identity;
  return image;
}

// A valid three-image set: an owner at 0x1000, a second owner at 0x8000 with two mappings, and a borrower of the first owner's
// bytes at 0x20000. No platform selector: every execution range is disjoint.
ExecutableImageSet base_set() {
  ExecutableImageSet set;
  set.cpu = CpuVariant::mc68000;
  set.images.push_back(owned(1, 0x100, {{0x1000, 0, 0x100}}));
  set.images.push_back(owned(2, 0x40, {{0x8000, 0, 0x20}, {0x9000, 0x20, 0x20}}, "test.second", ImageAuthority::static_proof));
  set.images.push_back(borrowed(3, {ImageId{1}, 0x10, 0x30}, {{0x20000, 0, 0x30}}));
  return set;
}

void expect(const ExecutableImageSet& set, ImageValidationError error, std::uint32_t index, const std::string& label) {
  const ImageValidation v = validate_executable_image_set(set);
  const bool ok = v.error == error && (error == ImageValidationError::none || v.image_index == index);
  check(ok, label + " -> " + std::string(image_validation_error_name(error)) + " @" + std::to_string(index) + " (got " +
                std::string(image_validation_error_name(v.error)) + " @" + std::to_string(v.image_index) + ")");
}

template <typename Mutate>
void mutant(ImageValidationError error, std::uint32_t index, const std::string& label, Mutate mutate) {
  ExecutableImageSet set = base_set();
  mutate(set);
  expect(set, error, index, label);
}

void valid_shapes() {
  expect(ExecutableImageSet{}, ImageValidationError::none, 0, "empty set is valid");
  expect(base_set(), ImageValidationError::none, 0, "owned + multi-mapping + source reference");
  {
    ExecutableImageSet set;
    set.images.push_back(owned(1, 1, {{0, 0, 1}}));
    expect(set, ImageValidationError::none, 0, "single one-byte image at execution base 0");
  }
  {  // a mapping that ends exactly at the top of the 32-bit execution space does not wrap
    ExecutableImageSet set;
    set.images.push_back(owned(1, 0x10, {{0xFFFFFFF0U, 0, 0x10}}));
    expect(set, ImageValidationError::none, 0, "mapping ending at 2^32 is valid");
  }
  {  // first_offset shifts the execution range: base + first_offset .. + length
    ExecutableImageSet set;
    set.images.push_back(owned(1, 0x20, {{0x1000, 0x10, 0x10}, {0x1000 - 0x10, 0x10, 0x10}}));
    expect(set, ImageValidationError::none, 0, "adjacent execution ranges from offset mappings are not overlapping");
  }
  {  // overlapping images are allowed only with a platform selector
    ExecutableImageSet set;
    set.images.push_back(owned(1, 0x40, {{0x4000, 0, 0x40}}));
    set.images.push_back(owned(2, 0x40, {{0x4000, 0, 0x40}}));
    set.platform_selector = true;
    expect(set, ImageValidationError::none, 0, "two images on the same range with a platform selector");
    set.platform_selector = false;
    expect(set, ImageValidationError::overlapping_mappings, 1, "two images on the same range without a selector");
  }
  {  // intra-image overlap is always rejected, selector or not
    ExecutableImageSet set;
    set.platform_selector = true;
    set.images.push_back(owned(1, 0x40, {{0x4000, 0, 0x40}}));
    set.images.push_back(owned(2, 0x40, {{0x8000, 0, 0x20}, {0x8010, 0, 0x20}}));
    expect(set, ImageValidationError::overlapping_mappings, 1, "intra-image overlap with a platform selector");
    set.platform_selector = false;
    expect(set, ImageValidationError::overlapping_mappings, 1, "intra-image overlap without a platform selector");
  }
}

void every_error() {
  // id_not_dense: wrong first id, zero id, a gap, a duplicate.
  mutant(ImageValidationError::id_not_dense, 0, "first id 2", [](auto& s) { s.images[0].id = ImageId{2}; });
  mutant(ImageValidationError::id_not_dense, 0, "first id 0", [](auto& s) { s.images[0].id = ImageId{0}; });
  mutant(ImageValidationError::id_not_dense, 1, "id gap", [](auto& s) { s.images[1].id = ImageId{4}; });
  mutant(ImageValidationError::id_not_dense, 1, "duplicate id", [](auto& s) { s.images[1].id = ImageId{1}; });
  mutant(ImageValidationError::id_not_dense, 1, "swapped order", [](auto& s) { std::swap(s.images[0], s.images[1]); s.images[0].id = ImageId{1}; });
  // unknown tags (static_cast of out-of-range values).
  mutant(ImageValidationError::unknown_authority, 1, "authority 3",
         [](auto& s) { s.images[1].provenance.authority = static_cast<ImageAuthority>(kImageAuthorityCount); });
  mutant(ImageValidationError::unknown_authority, 2, "authority 255",
         [](auto& s) { s.images[2].provenance.authority = static_cast<ImageAuthority>(255); });
  mutant(ImageValidationError::unknown_verification, 0, "verification 3",
         [](auto& s) { s.images[0].verification = static_cast<ImageVerification>(kImageVerificationCount); });
  mutant(ImageValidationError::unknown_verification, 2, "verification 200",
         [](auto& s) { s.images[2].verification = static_cast<ImageVerification>(200); });
  // producer names.
  mutant(ImageValidationError::invalid_producer_name, 0, "empty producer", [](auto& s) { s.images[0].provenance.producer.clear(); });
  mutant(ImageValidationError::invalid_producer_name, 1, "upper-case producer", [](auto& s) { s.images[1].provenance.producer = "Test.x"; });
  mutant(ImageValidationError::invalid_producer_name, 2, "dash in producer", [](auto& s) { s.images[2].provenance.producer = "test-x"; });
  mutant(ImageValidationError::invalid_producer_name, 0, "quote in producer", [](auto& s) { s.images[0].provenance.producer = "a\"b"; });
  mutant(ImageValidationError::invalid_producer_name, 0, "space in producer", [](auto& s) { s.images[0].provenance.producer = "a b"; });
  // bytes.
  mutant(ImageValidationError::no_bytes, 0, "no owned bytes and no source", [](auto& s) { s.images[0].bytes.clear(); });
  mutant(ImageValidationError::bytes_and_source, 2, "owned bytes and a source", [](auto& s) { s.images[2].bytes = {1, 2}; });
  // unresolved sources.
  mutant(ImageValidationError::unresolved_source, 2, "source id 0", [](auto& s) { s.images[2].source->source = ImageId{0}; });
  mutant(ImageValidationError::unresolved_source, 2, "source is itself", [](auto& s) { s.images[2].source->source = ImageId{3}; });
  mutant(ImageValidationError::unresolved_source, 2, "source out of range", [](auto& s) { s.images[2].source->source = ImageId{99}; });
  mutant(ImageValidationError::unresolved_source, 1, "forward reference", [](auto& s) {
    s.images[1].bytes.clear();
    s.images[1].source = ImageSourceReference{ImageId{3}, 0, 0x10};
  });
  mutant(ImageValidationError::unresolved_source, 3, "reference to a referencing image", [](auto& s) {
    s.images.push_back(borrowed(4, {ImageId{3}, 0, 0x10}, {{0x30000, 0, 0x10}}));
  });
  // source ranges.
  mutant(ImageValidationError::source_out_of_range, 2, "empty source", [](auto& s) { s.images[2].source->length = 0; });
  mutant(ImageValidationError::source_out_of_range, 2, "source past the end",
         [](auto& s) { s.images[2].source = ImageSourceReference{ImageId{1}, 0xF0, 0x20}; s.images[2].mappings = {{0x20000, 0, 0x20}}; });
  mutant(ImageValidationError::source_out_of_range, 2, "source offset overflow",
         [](auto& s) { s.images[2].source = ImageSourceReference{ImageId{1}, 0xFFFFFFFFU, 2}; s.images[2].mappings = {{0x20000, 0, 2}}; });
  // mappings.
  mutant(ImageValidationError::no_mappings, 1, "no mappings", [](auto& s) { s.images[1].mappings.clear(); });
  mutant(ImageValidationError::empty_mapping, 1, "empty second mapping", [](auto& s) { s.images[1].mappings[1].length = 0; });
  mutant(ImageValidationError::mapping_outside_bytes, 0, "mapping longer than bytes", [](auto& s) { s.images[0].mappings[0].length = 0x101; });
  mutant(ImageValidationError::mapping_outside_bytes, 2, "mapping longer than the source reference",
         [](auto& s) { s.images[2].mappings[0].length = 0x31; });
  mutant(ImageValidationError::mapping_outside_bytes, 1, "first_offset overflow",
         [](auto& s) { s.images[1].mappings[1].first_offset = 0xFFFFFFFFU; });
  mutant(ImageValidationError::mapping_wraps, 0, "base wraps", [](auto& s) { s.images[0].mappings[0].execution_base = 0xFFFFFF01U; });
  mutant(ImageValidationError::mapping_wraps, 1, "base + first_offset wraps",
         [](auto& s) { s.images[1].mappings[1].execution_base = 0xFFFFFFF0U; });
  mutant(ImageValidationError::overlapping_mappings, 1, "image 2 overlaps image 1",
         [](auto& s) { s.images[1].mappings[0].execution_base = 0x10F0; });
  mutant(ImageValidationError::overlapping_mappings, 1, "intra-image overlap",
         [](auto& s) { s.images[1].mappings[1].execution_base = 0x8000 - 0x10; });
  mutant(ImageValidationError::overlapping_mappings, 2, "borrower overlaps its source's range",
         [](auto& s) { s.images[2].mappings[0].execution_base = 0x1000; });
  // the selector admits inter-image overlap only.
  mutant(ImageValidationError::none, 0, "image 2 overlaps image 1 under a selector", [](auto& s) {
    s.images[1].mappings[0].execution_base = 0x10F0;
    s.platform_selector = true;
  });
  // The first offending image is reported, even when a later one is also wrong.
  mutant(ImageValidationError::no_mappings, 0, "first error wins", [](auto& s) {
    s.images[0].mappings.clear();
    s.images[2].source->source = ImageId{0};
  });
  // Checks are ordered: id before authority before verification before producer.
  mutant(ImageValidationError::id_not_dense, 0, "id checked before authority", [](auto& s) {
    s.images[0].id = ImageId{7};
    s.images[0].provenance.authority = static_cast<ImageAuthority>(9);
  });
  mutant(ImageValidationError::unknown_authority, 0, "authority checked before producer", [](auto& s) {
    s.images[0].provenance.authority = static_cast<ImageAuthority>(9);
    s.images[0].provenance.producer.clear();
  });

  // Every non-none error value is exercised above; the names are distinct and an out-of-range value is "unknown".
  std::vector<std::string> names;
  for (unsigned e = 0; e <= static_cast<unsigned>(ImageValidationError::overlapping_mappings); ++e)
    names.emplace_back(image_validation_error_name(static_cast<ImageValidationError>(e)));
  bool distinct = true;
  for (std::size_t i = 0; i < names.size(); ++i)
    for (std::size_t j = i + 1; j < names.size(); ++j) distinct = distinct && names[i] != names[j] && names[i] != "unknown";
  check(distinct && names.size() == 14U, "14 distinct error names");
  check(image_validation_error_name(static_cast<ImageValidationError>(14)) == "unknown", "error 14 is unknown");
  check(image_authority_name(static_cast<ImageAuthority>(3)) == "unknown", "authority 3 name is unknown");
  check(image_verification_name(static_cast<ImageVerification>(3)) == "unknown", "verification 3 name is unknown");
  check(image_authority_name(ImageAuthority::bounded_build_time_materialization) == "bounded_build_time_materialization" &&
            image_verification_name(ImageVerification::structural) == "structural",
        "known tag names");
}

void bytes_resolution() {
  const ExecutableImageSet set = base_set();
  const auto owner = executable_image_bytes(set, set.images[0]);
  check(owner.size() == 0x100 && owner.data() == set.images[0].bytes.data(), "owned bytes resolve to the image's own storage");
  const auto borrowedBytes = executable_image_bytes(set, set.images[2]);
  check(borrowedBytes.size() == 0x30 && borrowedBytes.data() == set.images[0].bytes.data() + 0x10,
        "a source reference resolves to the owner's subrange without a copy");
  check(executable_image_size(set.images[2]) == 0x30 && executable_image_size(set.images[1]) == 0x40, "executable_image_size");
}

void provenance_counts() {
  const ExecutableImageSet set = base_set();
  const ImageProvenanceCounts counts = count_image_provenance(set);
  check(counts.images == 3 && counts.by_authority[0] == 1 && counts.by_authority[1] == 2 && counts.by_authority[2] == 0,
        "counts by authority");
  const std::string json = format_image_provenance_json(counts);
  const std::string expected =
      "{\"images\":3,\"authority\":{\"immutable_input\":1,\"static_proof\":2,\"bounded_build_time_materialization\":0},"
      "\"producers\":{\"test.borrower\":1,\"test.owner\":1,\"test.second\":1}}";
  check(json == expected, "provenance JSON is exact: " + json);
  check(format_image_provenance_json(count_image_provenance(base_set())) == json, "provenance JSON is deterministic");
  check(format_image_provenance_json(count_image_provenance(ExecutableImageSet{})) ==
            "{\"images\":0,\"authority\":{\"immutable_input\":0,\"static_proof\":0,\"bounded_build_time_materialization\":0},"
            "\"producers\":{}}",
        "empty set provenance JSON");
  // Producers are reported in name order regardless of image order; evidence never appears.
  ExecutableImageSet reordered;
  reordered.images.push_back(owned(1, 4, {{0, 0, 4}}, "z.last", ImageAuthority::bounded_build_time_materialization));
  reordered.images.push_back(owned(2, 4, {{8, 0, 4}}, "a.first"));
  reordered.images.push_back(owned(3, 4, {{16, 0, 4}}, "z.last", ImageAuthority::bounded_build_time_materialization));
  reordered.images[0].provenance.evidence.push_back({"secret_digest", "00112233"});
  const std::string ordered = format_image_provenance_json(count_image_provenance(reordered));
  check(ordered ==
            "{\"images\":3,\"authority\":{\"immutable_input\":1,\"static_proof\":0,\"bounded_build_time_materialization\":2},"
            "\"producers\":{\"a.first\":1,\"z.last\":2}}",
        "producers sorted by name, evidence omitted: " + ordered);
  // An unknown authority (an invalid set) is counted as an image but under no authority.
  ExecutableImageSet unknown = reordered;
  unknown.images[1].provenance.authority = static_cast<ImageAuthority>(7);
  const ImageProvenanceCounts uc = count_image_provenance(unknown);
  check(uc.images == 3 && uc.by_authority[0] == 0 && uc.by_authority[2] == 2, "unknown authority is not counted under any authority");
}

void equality() {
  ExecutableImageSet a = base_set();
  ExecutableImageSet b = base_set();
  check(a == b, "identical sets compare equal");
  b.images[0].provenance.evidence[0].value = "other";
  check(!(a == b), "artifact equality includes evidence");
  b = base_set();
  b.platform_selector = true;
  check(!(a == b), "artifact equality includes the selector");
}

}  // namespace

int main() {
  valid_shapes();
  every_error();
  bytes_resolution();
  provenance_counts();
  equality();
  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("executable_image_test: all checks passed\n");
  return 0;
}
