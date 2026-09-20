#include "segarecomp/machine/genesis/frontend.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
int failures{};

void expect(bool condition, const char *message) {
  if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

using segarecomp::FrontendProgram;
using segarecomp::FrontendRejected;
using segarecomp::MappingClaim;

MappingClaim claim(std::string name, std::uint32_t begin, std::uint32_t end,
                   std::uint64_t image_begin, std::uint64_t image_end) {
  return {std::move(name), {{}, begin}, {{}, end}, {image_begin}, {image_end}};
}

FrontendProgram program(std::vector<std::uint8_t> image, std::uint32_t entry,
                        std::vector<MappingClaim> claims) {
  const auto length = static_cast<std::uint64_t>(image.size());
  return {{}, {"synthetic/SEG-004/rejections", std::move(image), length}, std::move(claims),
          {{{}, entry}}};
}

const FrontendRejected &reject(const FrontendProgram &input, const char *message) {
  static FrontendRejected failure{};
  const auto result = segarecomp::analyze_m68k_frontend(input);
  expect(std::holds_alternative<FrontendRejected>(result), message);
  failure = std::get<FrontendRejected>(result);
  return failure;
}

bool has_claims(const FrontendRejected &failure, std::initializer_list<const char *> names) {
  if (!failure.mapping_claims || failure.mapping_claims->size() != names.size()) return false;
  std::size_t index{};
  for (const char *name : names) {
    if (failure.mapping_claims->at(index++).name != name) return false;
  }
  return true;
}

bool claim_span(const MappingClaim &mapping, std::uint32_t target_begin, std::uint32_t target_end,
                std::uint64_t image_begin, std::uint64_t image_end) {
  return mapping.target_begin.space == segarecomp::TargetAddressSpace::m68k_program &&
         mapping.target_end.space == segarecomp::TargetAddressSpace::m68k_program &&
         mapping.target_begin.value == target_begin && mapping.target_end.value == target_end &&
         mapping.image_begin.value == image_begin && mapping.image_end.value == image_end;
}

bool no_decode_facts(const FrontendRejected &failure) {
  return !failure.image_offset && !failure.provenance && !failure.available_bytes &&
         !failure.requested_length && !failure.instruction_length;
}

void rejects_precedence_and_provenance() {
  const auto mapped = std::vector<MappingClaim>{claim("mapped", 0x100U, 0x102U, 0U, 2U)};
  auto failure = reject(program({0x4AU, 0xFCU}, 0x100U, mapped), "illegal input rejects");
  expect(failure.category == segarecomp::DirectFlowDiagnostic::illegal_instruction &&
             failure.source_address && failure.source_address->value == 0x100U &&
             failure.image_offset && failure.image_offset->value == 0U &&
             failure.provenance && failure.provenance->bytes == std::array<std::uint8_t, 2>{0x4AU, 0xFCU} &&
             failure.provenance->length.value == 2U && has_claims(failure, {"mapped"}) &&
             claim_span(failure.mapping_claims->front(), 0x100U, 0x102U, 0U, 2U) &&
             failure.available_bytes == std::optional<std::uint64_t>{2U} &&
             failure.requested_length == std::optional<std::uint32_t>{2U} &&
             failure.instruction_length == std::optional<std::uint32_t>{2U} &&
             !failure.direct.has_target,
         "ILLEGAL retains exact primary provenance and no target");

  failure = reject(program({0x72U, 0x01U}, 0x100U, mapped), "unsupported input rejects");
  expect(failure.category == segarecomp::DirectFlowDiagnostic::valid_but_unsupported_instruction &&
             failure.provenance && failure.provenance->bytes == std::array<std::uint8_t, 2>{0x72U, 0x01U} &&
             failure.provenance->length.value == 2U && has_claims(failure, {"mapped"}) &&
             !failure.direct.has_target,
         "unsupported primary retains exact bytes and no target");

  failure = reject(program({0x66U, 0x00U, 0x00U, 0x02U}, 0x100U,
                           {claim("mapped", 0x100U, 0x104U, 0U, 4U)}),
                   "zero-displacement extension rejects");
  expect(failure.category == segarecomp::DirectFlowDiagnostic::valid_but_unsupported_instruction &&
             failure.source_address && failure.source_address->value == 0x100U &&
             failure.image_offset && failure.image_offset->value == 0U && failure.provenance &&
             failure.provenance->bytes == std::array<std::uint8_t, 2>{0x66U, 0x00U} &&
             failure.provenance->length.value == 2U && failure.requested_length == std::optional<std::uint32_t>{4U} &&
             failure.instruction_length == std::optional<std::uint32_t>{2U} && !failure.direct.has_target,
         "zero-displacement keeps primary provenance, declared extension length, and null target");

  failure = reject(program({0x70U}, 0x100U, {claim("short", 0x100U, 0x101U, 0U, 1U)}),
                   "truncated primary rejects");
  expect(failure.category == segarecomp::DirectFlowDiagnostic::truncated_instruction &&
             failure.source_address && failure.source_address->value == 0x100U &&
             failure.image_offset && failure.image_offset->value == 0U && has_claims(failure, {"short"}) &&
             failure.available_bytes == std::optional<std::uint64_t>{1U} &&
             failure.requested_length == std::optional<std::uint32_t>{2U} && !failure.instruction_length &&
             !failure.provenance && !failure.direct.has_target,
         "truncated primary has bounded lengths but no bytes, instruction length, or target");

  failure = reject(program({0x66U, 0x00U, 0x00U, 0x02U}, 0x100U,
                           {claim("primary", 0x100U, 0x102U, 0U, 2U),
                            claim("adjacent", 0x102U, 0x104U, 2U, 4U)}),
                   "truncated extension rejects");
  expect(failure.category == segarecomp::DirectFlowDiagnostic::truncated_instruction &&
             failure.source_address && failure.source_address->value == 0x100U &&
             failure.image_offset && failure.image_offset->value == 0U && has_claims(failure, {"primary"}) &&
             claim_span(failure.mapping_claims->front(), 0x100U, 0x102U, 0U, 2U) &&
             failure.available_bytes == std::optional<std::uint64_t>{2U} &&
             failure.requested_length == std::optional<std::uint32_t>{4U} && !failure.instruction_length &&
             failure.provenance && failure.provenance->bytes == std::array<std::uint8_t, 2>{0x66U, 0x00U} &&
             failure.provenance->length.value == 2U && !failure.direct.has_target,
         "extension cannot borrow adjacent claim bytes or infer a target");
}

void rejects_mapping_and_target_failures() {
  auto failure = reject(program({0x70U, 0x01U}, 0x101U,
                                {claim("mapped", 0x100U, 0x102U, 0U, 2U)}),
                        "odd source rejects");
  expect(failure.category == segarecomp::DirectFlowDiagnostic::odd_instruction_address &&
             failure.source_address && failure.source_address->value == 0x101U &&
             !failure.mapping_claims && no_decode_facts(failure) && !failure.direct.has_target,
         "odd source precedes mapping resolution and all decode facts");

  failure = reject(program({0x70U, 0x01U}, 0x100U,
                           {claim("first", 0x100U, 0x102U, 0U, 2U),
                            claim("second", 0x100U, 0x102U, 0U, 2U)}),
                   "conflicting source rejects");
  expect(failure.category == segarecomp::DirectFlowDiagnostic::conflicting_address_mapping &&
             failure.source_address && failure.source_address->value == 0x100U &&
             has_claims(failure, {"first", "second"}) && no_decode_facts(failure) && !failure.direct.has_target,
         "conflict preserves claim ingestion order before a read");

  failure = reject(program({0x70U, 0x01U}, 0x200U,
                           {claim("mapped", 0x100U, 0x102U, 0U, 2U)}), "unmapped source rejects");
  expect(failure.category == segarecomp::DirectFlowDiagnostic::unmapped_instruction_address &&
             failure.source_address && failure.source_address->value == 0x200U &&
             failure.mapping_claims && failure.mapping_claims->empty() && no_decode_facts(failure) &&
             !failure.direct.has_target,
         "unmapped source retains present empty claims and no decode facts");

  failure = reject(program({0x4AU, 0xFCU}, 0x100U, {claim("bad", 0x100U, 0x103U, 0U, 2U)}),
                   "invalid claim rejects");
  expect(failure.category == segarecomp::DirectFlowDiagnostic::invalid_mapping_claim &&
             !failure.source_address && has_claims(failure, {"bad"}) && no_decode_facts(failure) &&
             !failure.direct.has_target,
         "invalid claim precedes source resolution and ILLEGAL decoding");

  failure = reject(program({0x60U, 0x01U}, 0x100U, {claim("mapped", 0x100U, 0x102U, 0U, 2U)}),
                   "odd target rejects");
  expect(failure.category == segarecomp::DirectFlowDiagnostic::odd_direct_target && failure.provenance &&
             failure.source_address && failure.source_address->value == 0x100U &&
             failure.image_offset && failure.image_offset->value == 0U &&
             failure.provenance->bytes == std::array<std::uint8_t, 2>{0x60U, 0x01U} &&
             failure.direct.has_target && failure.direct.target.value == 0x103U && has_claims(failure, {"mapped"}),
         "odd direct target retains branch bytes and target address");

  failure = reject(program({0x60U, 0x7EU}, 0x100U, {claim("mapped", 0x100U, 0x102U, 0U, 2U)}),
                   "unmapped target rejects");
  expect(failure.category == segarecomp::DirectFlowDiagnostic::unmapped_direct_target && failure.provenance &&
             failure.source_address && failure.source_address->value == 0x100U &&
             failure.image_offset && failure.image_offset->value == 0U &&
             failure.provenance->bytes == std::array<std::uint8_t, 2>{0x60U, 0x7EU} &&
             failure.direct.has_target && failure.direct.target.value == 0x180U &&
             has_claims(failure, {"mapped"}) && failure.direct.mapping_claims.size() == 1U &&
             failure.direct.mapping_claims.front().name == "mapped",
         "unmapped direct target retains branch provenance and source-claim provenance");
}
} // namespace

int main() {
  rejects_precedence_and_provenance();
  rejects_mapping_and_target_failures();
  return failures == 0 ? 0 : 1;
}
