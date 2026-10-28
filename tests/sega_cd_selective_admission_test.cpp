// SEG-097-T003 (ADR 0208): the Sega CD selective-admission seam with a FAKE producer (no model): application, validation, typed fallback, identity
// separation, one scoring per image, and the invariant that a proposal can only remove entries.
#include <cstdint>
#include <cstdio>
#include <functional>
#include <regex>
#include <set>
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

// moveq #1,d0 ; nop ; nop ; move.w #$AAAA,$00010020 ; nop ; rts          (word offsets 0 2 4 6 .. 14 16)
const auto kModule = words({0x7001, 0x4E71, 0x4E71, 0x33FC, 0xAAAA, 0x0001, 0x0020, 0x4E71, 0x4E75});
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
  p.images.images.push_back(image(1, kBoot, {{base, 0, static_cast<std::uint32_t>(kBoot.size())}}, processor == Processor::main ? "sega_cd.boot_ip" : "sega_cd.boot_sp"));
  p.entry_pc = base;
  p.roles.push_back({processor == Processor::main ? "boot_ip" : "boot_sp", false, {0}});
  return p;
}

BootImages program(std::vector<std::uint32_t> bases = {0x00020000U, 0x00030000U, 0x00040000U}) {
  BootImages images;
  images.main = boot(Processor::main, 0x00FF0000U);
  images.sub = boot(Processor::sub, 0x00006000U);
  std::vector<ImageMapping> mappings;
  for (const auto base : bases) mappings.push_back({base, 0, static_cast<std::uint32_t>(kModule.size())});
  images.sub.images.platform_selector = true;
  images.sub.images.images.push_back(image(2, kModule, std::move(mappings), "sega_cd.disc_module"));
  images.sub.roles.push_back({"module_a", true, {0}});
  return images;
}

std::set<std::string> functions(const std::string &c) {
  std::set<std::string> out;
  const std::regex pattern(R"(static int (scd_p[01]_(?:m\d+_)?(?:map\d+_)?i_[0-9A-F]+)\()");
  for (std::sregex_iterator it(c.begin(), c.end(), pattern), end; it != end; ++it) out.insert((*it)[1]);
  return out;
}

struct Fake {
  std::function<RegionProposal(std::span<const std::uint8_t>, std::span<const std::uint32_t>)> body;
  int calls = 0;
  std::vector<std::size_t> sizes;
  SelectiveAdmissionRequest request(std::string identity = "fake-policy-v1") {
    SelectiveAdmissionRequest r;
    r.policy_identity = std::move(identity);
    r.propose = [this](std::span<const std::uint8_t> bytes, std::span<const std::uint32_t> seeds) {
      ++calls;
      sizes.push_back(bytes.size());
      return body(bytes, seeds);
    };
    return r;
  }
};

RegionProposal ranges(std::vector<std::pair<std::uint32_t, std::uint32_t>> r, std::uint32_t units = 1) {
  RegionProposal p;
  p.ok = true;
  p.ranges = std::move(r);
  p.units = units;
  p.selected = 1;
  return p;
}

}  // namespace

int main() {
  std::uint32_t distinct_broad = 0;  // distinct source-relative instruction offsets of the broad emission (all mappings of an image counted once)
  const auto broad = emit_sega_cd_processors(program());
  check(broad.error == EmissionError::none && !broad.admission.requested, "baseline: the broad emission reports no experimental request");

  // 1. A proposal that contains everything changes nothing.
  {
    Fake fake;
    fake.body = [](auto bytes, auto) { return ranges({{0, static_cast<std::uint32_t>(bytes.size())}}); };
    const auto request = fake.request();
    SegaCdEmitOptions options;
    options.selective = &request;
    const auto e = emit_sega_cd_processors(program(), options);
    check(e.admission.requested && e.admission.admitted && e.admission.reason == "none", "a full proposal is applied");
    check(e.c_source == broad.c_source && e.overlay_source == broad.overlay_source, "a full proposal emits exactly the broad text");
    check(e.admission.broad_entries == e.admission.admitted_entries && !e.admission.k_sha256.empty(), "the counts agree and the admitted set has a digest");
    distinct_broad = e.admission.broad_entries;
    check(distinct_broad < broad.stats[0].compiled + broad.stats[1].compiled, "entries are SOURCE-RELATIVE offsets: the module's three mappings count once, not three times");
    check(fake.calls == 3, "one scoring per IMAGE: 3 images (IP, SP, one module with 3 mappings) -> 3 proposals, not 5 (got " + std::to_string(fake.calls) + ")");
  }
  // 2. A partial proposal removes instructions and nothing else.
  {
    Fake fake;
    fake.body = [](auto bytes, auto) {
      return bytes.size() == kModule.size() ? ranges({{0, 6}}) /* moveq, nop, nop */ : ranges({{0, static_cast<std::uint32_t>(bytes.size())}});
    };
    const auto request = fake.request();
    SegaCdEmitOptions options;
    options.selective = &request;
    const auto e = emit_sega_cd_processors(program(), options);
    check(e.admission.admitted && e.admission.admitted_entries < e.admission.broad_entries, "a partial proposal admits fewer entries than broad");
    const auto before = functions(broad.c_source), after = functions(e.c_source);
    bool subset = true;
    for (const auto &name : after) subset = subset && before.count(name) != 0U;
    check(subset && after.size() < before.size(), "the selective functions are a strict subset of the broad ones: a proposal can only remove entries");
    check(e.c_source.find("scd_p1_m2_map0_i_00020006") == std::string::npos && e.c_source.find("scd_p1_m2_map0_i_00020000") != std::string::npos,
          "an omitted instruction has no function, the proposed ones keep theirs");
    check(e.c_source.find("default: return scd_overlay_dispatch") != std::string::npos, "an omitted entry falls to the typed uncompiled-entry stop of the unchanged dispatch");
    bool same_inventory = e.images.size() == broad.images.size();
    for (std::size_t i = 0; same_inventory && i < e.images.size(); ++i)
      same_inventory = e.images[i].image_id == broad.images[i].image_id && e.images[i].start == broad.images[i].start && e.images[i].length == broad.images[i].length &&
                       e.images[i].roots == broad.images[i].roots && e.images[i].producer == broad.images[i].producer && e.images[i].name == broad.images[i].name;
    check(same_inventory, "the image and mapping inventory is identical to broad (a proposal never touches authority)");
    // determinism
    const auto again = emit_sega_cd_processors(program(), options);
    check(again.c_source == e.c_source && again.admission.k_sha256 == e.admission.k_sha256, "the same proposal gives the same text and admitted-set digest");
  }
  // 3. Typed fallbacks: the broad text, the exact reason, no experimental claim.
  const struct { const char *name; std::function<RegionProposal(std::span<const std::uint8_t>, std::span<const std::uint32_t>)> body; const char *reason; } failing[] = {
      {"model_identity", [](auto, auto) { RegionProposal p; p.failure = "model_identity"; return p; }, "model_identity"},
      {"rom_size", [](auto, auto) { RegionProposal p; p.failure = "rom_size"; return p; }, "rom_size"},
      {"non_finite_score", [](auto, auto) { RegionProposal p; p.failure = "non_finite_score"; return p; }, "non_finite_score"},
      {"empty", [](auto, auto) { return ranges({}); }, "empty_proposal"},
      {"odd range", [](auto, auto) { return ranges({{1, 4}}); }, "validator_rejected:range_malformed"},
      {"empty range", [](auto, auto) { return ranges({{4, 4}}); }, "validator_rejected:range_malformed"},
      {"outside the image", [](auto bytes, auto) { return ranges({{0, static_cast<std::uint32_t>(bytes.size()) + 2U}}); }, "validator_rejected:range_outside_image"},
      {"unordered", [](auto, auto) { return ranges({{2, 4}, {0, 2}}); }, "validator_rejected:range_order"},
      {"seed outside the proposal", [](auto bytes, auto) { return ranges({{2, static_cast<std::uint32_t>(bytes.size())}}); }, "validator_rejected:seed_not_in_proposal"},
  };
  for (const auto &f : failing) {
    Fake fake;
    fake.body = f.body;
    const auto request = fake.request();
    SegaCdEmitOptions options;
    options.selective = &request;
    const auto e = emit_sega_cd_processors(program(), options);
    check(e.admission.requested && !e.admission.admitted && e.admission.reason == f.reason && e.c_source == broad.c_source && e.admission.admitted_entries == 0U &&
              e.admission.k_sha256.empty() && !e.admission.failed_image.empty() && e.admission.broad_entries == distinct_broad && e.admission.images.size() == 3U,
          std::string("fallback (") + f.name + "): broad text, reason " + f.reason + ", failed image named, no admitted set claimed (got " + e.admission.reason + ")");
  }
  // 4. Identity: the request enters the program identity; a fallback never aliases a selective build; the default is untouched.
  {
    Fake good, bad;
    good.body = [](auto bytes, auto) { return ranges({{0, static_cast<std::uint32_t>(bytes.size())}}); };
    bad.body = [](auto, auto) { RegionProposal p; p.failure = "rom_size"; return p; };
    const auto good_request = good.request("policy-A"), other_request = good.request("policy-B"), bad_request = bad.request("policy-A");
    BootImages images = program();
    DiscImage none;
    SegaCdEmitOptions plain, a, b, c;
    a.selective = &good_request;
    b.selective = &other_request;
    c.selective = &bad_request;
    const auto id_plain = emit_sega_cd_program(none, images, {}, plain).program_identity;
    const auto id_a = emit_sega_cd_program(none, images, {}, a).program_identity;
    const auto id_b = emit_sega_cd_program(none, images, {}, b).program_identity;
    const auto id_c = emit_sega_cd_program(none, images, {}, c).program_identity;
    check(id_plain != id_a && id_a != id_b, "the policy identity enters the program identity: Compatibility, policy A and policy B are three identities");
    check(id_c == id_a, "a fallback build of request A has the identity of request A (the identity is the REQUEST; the outcome is reported, and K is a function of the inputs)");
    check(emit_sega_cd_program(none, images, {}, plain).program_identity == id_plain, "the default identity is stable");
  }
  // 5. validate_region_proposal on its own
  {
    check(validate_region_proposal(ranges({{0, 8}}), 8, {}).empty(), "validate: a full range is valid");
    const std::uint32_t seed[] = {4};
    check(validate_region_proposal(ranges({{0, 4}}), 8, seed) == "validator_rejected:seed_not_in_proposal", "validate: a seed outside the ranges is rejected");
    check(ranges_contain({{0, 4}, {8, 12}}, 8) && !ranges_contain({{0, 4}, {8, 12}}, 6) && !ranges_contain({{0, 4}, {8, 12}}, 12), "ranges_contain: half-open membership");
  }
  std::printf("%d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
