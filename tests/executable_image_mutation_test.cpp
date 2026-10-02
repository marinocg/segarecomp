// SEG-028-T006 (ADR 0077): mutation and replacement harness for the executable-image boundary on both CPUs.
//
// Mutants of producer output (SMS cartridge banks and Genesis materialized images on the Z80; the Genesis cartridge + ADR 0049 alias
// set on the M68K) must each be detected by the generic validator, by the Z80 projection, or by an inequality of the projected Z80
// ImageSet. A mutant that cannot change emission (evidence, producer name, a different valid authority) is asserted to be equivalent:
// that is the intended contract (provenance never alters generated code).
//
// Replacement proof (ADR 0073 decision 8): a scripted stand-in producer declaring {static_proof, "test.z80_replay"} replays known
// epochs into a fresh registry; it yields an equal registry, an identical projected ImageSet and byte-identical emitted files, and
// its provenance differs from the materializer's only in authority and producer name. Determinism: production, projection and emission
// repeated twice give identical bytes. Synthetic, project-authored fixtures only.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "segarecomp/codegen/c11/z80_executable_image.hpp"
#include "segarecomp/machine/genesis/frontend.hpp"
#include "segarecomp/machine/genesis/z80_images.hpp"
#include "segarecomp/machine/master_system/cartridge.hpp"
#include "segarecomp/machine/master_system/emit.hpp"
#include "segarecomp/machine/master_system/image_set.hpp"

namespace {

using namespace segarecomp;
namespace sms = segarecomp::machine::master_system;
namespace gz80 = segarecomp::machine::genesis::z80;
namespace cz80 = segarecomp::codegen::z80;

int failures = 0;
int detected = 0;
int equivalent = 0;

void check(bool ok, const std::string& label) {
  std::printf("%-5s %s\n", ok ? "ok" : "FAIL", label.c_str());
  if (!ok) ++failures;
}

// ---- fixtures ---------------------------------------------------------------------------------------------------------------------

std::vector<std::uint8_t> make_rom(std::size_t size) {
  std::uint64_t state = 0x5EC028ULL + size;
  std::vector<std::uint8_t> rom(size);
  for (auto& b : rom) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    b = static_cast<std::uint8_t>(state >> 33);
  }
  std::memcpy(&rom[0x7FF0], "TMR SEGA", 8);
  rom[0x7FF8] = rom[0x7FF9] = 0;
  rom[0x7FFF] = static_cast<std::uint8_t>((4U << 4) | 0x0C);
  return rom;
}

sms::IngestOptions sega() {
  sms::IngestOptions o;
  o.declarations.push_back({"sega", sms::DeclarationSource::build_option});
  return o;
}

gz80::Epoch epoch(unsigned seed, std::size_t written_offset, std::size_t length) {
  gz80::Epoch e;
  for (std::size_t i = 0; i < gz80::kRamBytes; ++i) e.ram[i] = static_cast<std::uint8_t>((i * 37 + seed * 11 + 3) & 0xFF);
  for (std::size_t i = written_offset; i < written_offset + length; ++i)
    e.written[i >> 3] = static_cast<std::uint8_t>(e.written[i >> 3] | (1U << (i & 7)));
  return e;
}

std::vector<gz80::Epoch> known_epochs() { return {epoch(1, 0, 96), epoch(2, 512, 40), epoch(3, 1024, 200)}; }

// The "materialized" registry: what the build-time fixed point registers for these activations (a re-activation included).
gz80::Registry materialized_registry() {
  gz80::Registry registry;
  std::uint32_t bound = 0;
  const auto epochs = known_epochs();
  (void)registry.add(epochs[0], bound);
  (void)registry.add(epochs[1], bound);
  (void)registry.add(epochs[0], bound);  // re-activation: existing
  (void)registry.add(epochs[2], bound);
  return registry;
}

// A scripted stand-in producer: replays known epochs into a fresh registry (no guest execution) and declares itself static_proof.
struct ReplayProducer {
  gz80::ImageProducer producer{ImageAuthority::static_proof, "test.z80_replay"};
  gz80::Registry replay(const std::vector<gz80::Epoch>& epochs) const {
    gz80::Registry registry;
    std::uint32_t bound = 0;
    for (const auto& e : epochs) (void)registry.add(e, bound);
    return registry;
  }
};

const std::vector<std::uint8_t> m68k_image = [] {
  std::vector<std::uint8_t> bytes(0x80, 0x4EU);
  for (std::size_t i = 1; i < bytes.size(); i += 2) bytes[i] = 0x71U;  // NOP padding
  return bytes;
}();

FrontendProgram m68k_program() {
  FrontendProgram program{};
  program.profile = M68kFrontendProfile::general_startup;
  program.image = {"synthetic/SEG-028-T006/mutation", m68k_image, m68k_image.size()};
  program.mapping_claims = {{"raw_cartridge_rom",
                             {TargetAddressSpace::m68k_program, 0x1000U},
                             {TargetAddressSpace::m68k_program, static_cast<std::uint32_t>(0x1000U + m68k_image.size())},
                             {0U},
                             {m68k_image.size()}}};
  program.startup_ingress = M68kStartupIngress{{TargetAddressSpace::m68k_program, 0x1000U}, 0x00FF1000U};
  (void)apply_genesis_immutable_copy_alias(program, 0x00FF0400U, 0x1040U, 0x20U);
  (void)apply_genesis_immutable_copy_alias(program, 0x00FF0800U, 0x1010U, 0x10U);
  return program;
}

// ---- detection --------------------------------------------------------------------------------------------------------------------

bool same_set(const cz80::ImageSet& a, const cz80::ImageSet& b) {
  if (a.images.size() != b.images.size()) return false;
  for (std::size_t i = 0; i < a.images.size(); ++i) {
    const auto& x = a.images[i];
    const auto& y = b.images[i];
    if (x.identity != y.identity || x.kind != y.kind || x.live_bytes != y.live_bytes || x.bytes != y.bytes ||
        x.windows.size() != y.windows.size())
      return false;
    for (std::size_t w = 0; w < x.windows.size(); ++w)
      if (x.windows[w].base != y.windows[w].base || x.windows[w].first_offset != y.windows[w].first_offset ||
          x.windows[w].length != y.windows[w].length)
        return false;
  }
  return true;
}

enum class Detection { validator, adapter, projection_differs, equivalent };

const char* detection_name(Detection d) {
  switch (d) {
  case Detection::validator: return "validator";
  case Detection::adapter: return "adapter";
  case Detection::projection_differs: return "projection_differs";
  case Detection::equivalent: return "equivalent";
  }
  return "?";
}

struct Z80Subject {
  std::string name;
  ExecutableImageSet set;
  std::vector<cz80::ImageKind> kinds;
  cz80::ImageSet reference;  // the subject's projected ImageSet
};

Detection classify(const ExecutableImageSet& mutated, const std::vector<cz80::ImageKind>& kinds, const cz80::ImageSet& reference,
                   std::string& detail) {
  const ImageValidation v = validate_executable_image_set(mutated);
  if (!v.ok()) {
    detail = std::string(image_validation_error_name(v.error));
    return Detection::validator;
  }
  const cz80::ImageProjection projected = cz80::project_executable_images(mutated, kinds);
  if (!projected.ok()) {
    detail = projected.error;
    return Detection::adapter;
  }
  return same_set(projected.set, reference) ? Detection::equivalent : Detection::projection_differs;
}

void z80_mutant(const Z80Subject& subject, const std::string& label, Detection expected,
                const std::function<void(ExecutableImageSet&, std::vector<cz80::ImageKind>&)>& mutate, const std::string& needle = {}) {
  ExecutableImageSet set = subject.set;
  std::vector<cz80::ImageKind> kinds = subject.kinds;
  mutate(set, kinds);
  std::string detail;
  const Detection got = classify(set, kinds, subject.reference, detail);
  const bool ok = got == expected && (needle.empty() || detail.find(needle) != std::string::npos);
  check(ok, subject.name + " / " + label + ": " + detection_name(got) + (detail.empty() ? "" : " (" + detail + ")"));
  if (got == Detection::equivalent) ++equivalent;
  else ++detected;
}

void z80_mutants(const Z80Subject& s) {
  using D = Detection;
  using Set = ExecutableImageSet;
  using Kinds = std::vector<cz80::ImageKind>;
  check(s.set.images.size() >= 2 && validate_executable_image_set(s.set).ok(), s.name + ": unmutated subject validates");
  {
    std::string detail;
    check(classify(s.set, s.kinds, s.reference, detail) == D::equivalent, s.name + ": unmutated subject projects to its reference");
  }
  // identity order and density
  z80_mutant(s, "id gap", D::validator, [](Set& x, Kinds&) { x.images[1].id = ImageId{7}; }, "id_not_dense");
  z80_mutant(s, "ids swapped", D::validator, [](Set& x, Kinds&) { std::swap(x.images[0].id, x.images[1].id); }, "id_not_dense");
  z80_mutant(s, "id zero", D::validator, [](Set& x, Kinds&) { x.images[0].id = ImageId{0}; }, "id_not_dense");
  z80_mutant(s, "images reordered (renumbered)", D::projection_differs, [](Set& x, Kinds& k) {
    std::swap(x.images[0], x.images[1]);
    x.images[0].id = ImageId{1};
    x.images[1].id = ImageId{2};
    std::swap(k[0], k[1]);
  });
  z80_mutant(s, "last image dropped", D::projection_differs, [](Set& x, Kinds& k) { x.images.pop_back(); k.pop_back(); });
  // mappings
  z80_mutant(s, "mapping wraps 2^32", D::validator, [](Set& x, Kinds&) { x.images[0].mappings[0].execution_base = 0xFFFFFF00U; },
             "mapping_wraps");
  z80_mutant(s, "mapping base beyond 16 bits", D::adapter, [](Set& x, Kinds&) { x.images[0].mappings[0].execution_base = 0x10000U; },
             "16-bit logical space");
  z80_mutant(s, "mapping first_offset overflow", D::validator,
             [](Set& x, Kinds&) { x.images[1].mappings[0].first_offset = 0xFFFFFFFFU; }, "mapping_outside_bytes");
  z80_mutant(s, "empty mapping", D::validator, [](Set& x, Kinds&) { x.images[1].mappings[0].length = 0; }, "empty_mapping");
  z80_mutant(s, "mapping out of bytes", D::validator, [](Set& x, Kinds&) { ++x.images[1].mappings[0].length; },
             "mapping_outside_bytes");
  z80_mutant(s, "no mappings", D::validator, [](Set& x, Kinds&) { x.images[1].mappings.clear(); }, "no_mappings");
  z80_mutant(s, "intra-image overlap", D::validator,
             [](Set& x, Kinds&) { x.images[1].mappings.push_back(x.images[1].mappings[0]); }, "overlapping_mappings");
  z80_mutant(s, "selector removed (images overlap)", D::validator, [](Set& x, Kinds&) { x.platform_selector = false; },
             "overlapping_mappings");
  z80_mutant(s, "mapping moved", D::projection_differs, [](Set& x, Kinds&) { x.images[1].mappings.back().execution_base += 2U; });
  // verification
  z80_mutant(s, "verification byte_identity", D::adapter,
             [](Set& x, Kinds&) { x.images[1].verification = ImageVerification::byte_identity; }, "byte_identity");
  z80_mutant(s, "verification structural<->none", D::projection_differs, [](Set& x, Kinds&) {
    auto& v = x.images[1].verification;
    v = v == ImageVerification::structural ? ImageVerification::none : ImageVerification::structural;
  });
  z80_mutant(s, "verification unknown", D::validator,
             [](Set& x, Kinds&) { x.images[0].verification = static_cast<ImageVerification>(3); }, "unknown_verification");
  // provenance
  z80_mutant(s, "authority unknown", D::validator,
             [](Set& x, Kinds&) { x.images[0].provenance.authority = static_cast<ImageAuthority>(3); }, "unknown_authority");
  z80_mutant(s, "producer empty", D::validator, [](Set& x, Kinds&) { x.images[0].provenance.producer.clear(); },
             "invalid_producer_name");
  z80_mutant(s, "producer invalid", D::validator, [](Set& x, Kinds&) { x.images[1].provenance.producer = "Bad Name"; },
             "invalid_producer_name");
  // Equivalent by contract: provenance never alters emission.
  z80_mutant(s, "producer renamed (valid)", D::equivalent, [](Set& x, Kinds&) { x.images[0].provenance.producer = "test.other"; });
  z80_mutant(s, "authority changed (valid)", D::equivalent,
             [](Set& x, Kinds&) { x.images[1].provenance.authority = ImageAuthority::static_proof; });
  z80_mutant(s, "evidence changed", D::equivalent, [](Set& x, Kinds&) {
    x.images[0].provenance.evidence.clear();
    x.images[1].provenance.evidence.push_back({"note", "mutant"});
  });
  // bytes and source references
  z80_mutant(s, "byte flipped", D::projection_differs, [](Set& x, Kinds&) { x.images[1].bytes[0] ^= 0x01U; });
  z80_mutant(s, "bytes dropped", D::validator, [](Set& x, Kinds&) { x.images[1].bytes.clear(); }, "no_bytes");
  z80_mutant(s, "bytes and source", D::validator,
             [](Set& x, Kinds&) { x.images[1].source = ImageSourceReference{ImageId{1}, 0, 1}; }, "bytes_and_source");
  const auto to_source = [](ImageSourceReference ref) {
    return [ref](Set& x, Kinds&) {
      x.images[1].bytes.clear();
      x.images[1].source = ref;
    };
  };
  z80_mutant(s, "source id 0", D::validator, to_source({ImageId{0}, 0, 1}), "unresolved_source");
  z80_mutant(s, "source forward reference", D::validator, to_source({ImageId{3}, 0, 1}), "unresolved_source");
  z80_mutant(s, "source self reference", D::validator, to_source({ImageId{2}, 0, 1}), "unresolved_source");
  z80_mutant(s, "source out of range", D::validator, to_source({ImageId{1}, 0, 0x7FFFFFFFU}), "source_out_of_range");
  z80_mutant(s, "source to a referencing image", D::validator, [](Set& x, Kinds&) {
    const auto size = static_cast<std::uint32_t>(x.images[0].bytes.size());
    x.images[1].bytes.clear();
    x.images[1].source = ImageSourceReference{ImageId{1}, 0, size};
    x.images[1].mappings = {{x.images[1].mappings[0].execution_base, 0, size}};
    x.images[2].bytes.clear();
    x.images[2].source = ImageSourceReference{ImageId{2}, 0, 1};
  }, "unresolved_source");
  // CPU and kind binding
  z80_mutant(s, "cpu mismatch", D::adapter, [](Set& x, Kinds&) { x.cpu = CpuVariant::mc68000; }, "not a Z80 set");
  z80_mutant(s, "kind count mismatch", D::adapter, [](Set&, Kinds& k) { k.pop_back(); }, "kind count");
  z80_mutant(s, "kind flipped", D::projection_differs, [](Set&, Kinds& k) {
    k[1] = k[1] == cz80::ImageKind::banked ? cz80::ImageKind::invariant : cz80::ImageKind::banked;
  });
}

void m68k_mutants() {
  const auto produced = genesis_m68k_executable_images(m68k_program());
  check(produced && produced->set.images.size() == 3, "m68k: producer yields cartridge + two alias images");
  if (!produced || produced->set.images.size() != 3) return;
  const ExecutableImageSet& base = produced->set;
  check(validate_executable_image_set(base).ok(), "m68k: unmutated subject validates");
  const auto mutant = [&](const std::string& label, const std::function<void(ExecutableImageSet&)>& mutate,
                          ImageValidationError expected, std::uint32_t index) {
    ExecutableImageSet set = base;
    mutate(set);
    const ImageValidation v = validate_executable_image_set(set);
    check(v.error == expected && v.image_index == index,
          "m68k / " + label + ": validator " + std::string(image_validation_error_name(v.error)) + " @" + std::to_string(v.image_index));
    ++detected;
  };
  using E = ImageValidationError;
  mutant("id gap", [](auto& x) { x.images[2].id = ImageId{9}; }, E::id_not_dense, 2);
  mutant("ids swapped", [](auto& x) { std::swap(x.images[1].id, x.images[2].id); }, E::id_not_dense, 1);
  mutant("cartridge mapping wraps", [](auto& x) { x.images[0].mappings[0].execution_base = 0xFFFFFFC0U; }, E::mapping_wraps, 0);
  mutant("alias mapping empty", [](auto& x) { x.images[1].mappings[0].length = 0; }, E::empty_mapping, 1);
  mutant("alias mapping beyond its source", [](auto& x) { x.images[1].mappings[0].length += 2U; }, E::mapping_outside_bytes, 1);
  mutant("alias mapping overlaps the other alias", [](auto& x) { x.images[2].mappings[0].execution_base = 0x00FF0410U; },
         E::overlapping_mappings, 2);
  mutant("alias mapping overlaps the cartridge", [](auto& x) { x.images[1].mappings[0].execution_base = 0x1000U; },
         E::overlapping_mappings, 1);
  mutant("authority unknown", [](auto& x) { x.images[1].provenance.authority = static_cast<ImageAuthority>(5); },
         E::unknown_authority, 1);
  mutant("producer invalid", [](auto& x) { x.images[2].provenance.producer = "genesis-copy"; }, E::invalid_producer_name, 2);
  mutant("source id 0", [](auto& x) { x.images[1].source->source = ImageId{0}; }, E::unresolved_source, 1);
  mutant("source forward reference", [](auto& x) { x.images[1].source->source = ImageId{3}; }, E::unresolved_source, 1);
  mutant("source to a referencing image", [](auto& x) { x.images[2].source->source = ImageId{2}; }, E::unresolved_source, 2);
  mutant("source out of range", [](auto& x) { x.images[1].source->offset = 0x70U; }, E::source_out_of_range, 1);
  mutant("source with owned bytes", [](auto& x) { x.images[1].bytes = {0x4EU, 0x71U}; }, E::bytes_and_source, 1);
  {  // CPU mismatch: the M68K set offered to the Z80 projection is refused
    const std::vector<cz80::ImageKind> kinds(base.images.size(), cz80::ImageKind::banked);
    const auto projected = cz80::project_executable_images(base, kinds);
    check(!projected.ok() && projected.error.find("not a Z80 set") != std::string::npos,
          "m68k / offered to the Z80 projection: adapter (" + projected.error + ")");
    ++detected;
  }
  // The M68K consumer (the alias identity loop) admits the alias image only as `static_proof` with a source reference; a producer
  // that described an alias as an owned or immutable_input image would not create alias identities. The descriptor round trip is
  // covered by tests/genesis_m68k_executable_image_test.cpp.
}

// ---- replacement proof and determinism --------------------------------------------------------------------------------------------

std::map<std::string, std::string> read_tree(const std::filesystem::path& dir) {
  std::map<std::string, std::string> files;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
    if (!entry.is_regular_file()) continue;
    std::ifstream in(entry.path(), std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    files[std::filesystem::relative(entry.path(), dir).generic_string()] = text.str();
  }
  return files;
}

std::filesystem::path scratch(const std::string& name) {
  const auto dir = std::filesystem::temp_directory_path() / ("seg028_mutation_" + std::to_string(std::random_device{}()) + "_" + name);
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

std::map<std::string, std::string> emit_registry_tree(const gz80::Registry& registry, const std::string& name, std::string& error) {
  gz80::EmitRequest request;
  request.directory = scratch(name);
  const gz80::EmitOutcome outcome = gz80::emit_registry(registry, request);
  error = outcome.error;
  auto files = read_tree(request.directory);
  std::filesystem::remove_all(request.directory);
  return files;
}

std::map<std::string, std::string> emit_sms_tree(const std::vector<std::uint8_t>& rom, const std::string& name, std::string& error,
                                                 ImageProvenanceCounts& provenance) {
  sms::EmitRequest request;
  request.directory = scratch(name);
  const sms::EmitOutcome outcome = sms::emit_cartridge(rom, sega(), request);
  error = outcome.error;
  provenance = outcome.provenance;
  auto files = read_tree(request.directory);
  std::filesystem::remove_all(request.directory);
  return files;
}

void replacement_proof() {
  const gz80::Registry materialized = materialized_registry();
  const ReplayProducer stand_in;
  const gz80::Registry replayed = stand_in.replay(known_epochs());
  check(materialized.images().size() == 3 && gz80::registries_equal(materialized, replayed),
        "replacement: the stand-in producer's registry equals the materialized registry");
  check(!gz80::registries_equal(materialized, stand_in.replay({known_epochs()[1], known_epochs()[0], known_epochs()[2]})),
        "replacement: a registry with a different activation order is not equal (registries_equal is order-sensitive)");

  const ExecutableImageSet produced = gz80::executable_images(materialized);
  const ExecutableImageSet replaced = gz80::executable_images(replayed, stand_in.producer);
  const std::vector<cz80::ImageKind> kinds(replaced.images.size(), cz80::ImageKind::banked);
  const auto projected = cz80::project_executable_images(replaced, kinds);
  check(projected.ok() && same_set(projected.set, gz80::build_image_set(materialized)),
        "replacement: the stand-in's projected ImageSet is identical to the materialized one");

  // The two artifacts differ only in provenance authority and producer name.
  ExecutableImageSet normalized = replaced;
  for (auto& image : normalized.images) {
    image.provenance.authority = ImageAuthority::bounded_build_time_materialization;
    image.provenance.producer = "genesis.z80_materializer";
  }
  check(normalized == produced, "replacement: the executable images differ only in authority and producer name");
  const ImageProvenanceCounts a = count_image_provenance(produced);
  const ImageProvenanceCounts b = count_image_provenance(replaced);
  check(a.images == 3 && b.images == 3 && a.by_authority[2] == 3 && b.by_authority[1] == 3 && a.by_authority[1] == 0 &&
            b.by_authority[2] == 0 && a.by_producer == std::map<std::string, std::uint32_t>{{"genesis.z80_materializer", 3}} &&
            b.by_producer == std::map<std::string, std::uint32_t>{{"test.z80_replay", 3}},
        "replacement: provenance counts differ only in authority and producer (" + format_image_provenance_json(b) + ")");

  std::string e1, e2;
  const auto materialized_files = emit_registry_tree(materialized, "materialized", e1);
  const auto replayed_files = emit_registry_tree(replayed, "replayed", e2);
  check(e1.empty() && e2.empty() && !materialized_files.empty() && materialized_files == replayed_files,
        "replacement: emit_registry output is byte-identical (" + std::to_string(materialized_files.size()) + " files)");
}

void determinism() {
  {
    std::string e1, e2;
    const auto first = emit_registry_tree(materialized_registry(), "det_genesis_1", e1);
    const auto second = emit_registry_tree(materialized_registry(), "det_genesis_2", e2);
    check(e1.empty() && e2.empty() && !first.empty() && first == second,
          "determinism: Genesis production + projection + emission twice is byte-identical");
  }
  {
    const auto rom = make_rom(0x8000);
    std::string e1, e2;
    ImageProvenanceCounts p1, p2;
    const auto first = emit_sms_tree(rom, "det_sms_1", e1, p1);
    const auto second = emit_sms_tree(rom, "det_sms_2", e2, p2);
    check(e1.empty() && e2.empty() && !first.empty() && first == second && p1 == p2,
          "determinism: SMS production + projection + emission twice is byte-identical");
    check(format_image_provenance_json(p1) ==
              "{\"images\":3,\"authority\":{\"immutable_input\":3,\"static_proof\":0,\"bounded_build_time_materialization\":0},"
              "\"producers\":{\"sms.cartridge_bank\":3}}",
          "SMS emit outcome reports the sanitized provenance of the emitted images: " + format_image_provenance_json(p1));
  }
  {
    const auto a = genesis_m68k_executable_images(m68k_program());
    const auto b = genesis_m68k_executable_images(m68k_program());
    check(a && b && a->set == b->set && a->claim_index == b->claim_index, "determinism: the M68K producer is a pure function");
  }
}

}  // namespace

int main() {
  {
    const auto rom = make_rom(0x20000);
    const auto cartridge = sms::ingest_cartridge(rom, sega());
    Z80Subject subject{"sms sega 8 banks", sms::executable_images(cartridge), {}, sms::build_image_set(cartridge)};
    subject.kinds = sms::image_kinds(subject.set);
    z80_mutants(subject);
  }
  {
    const gz80::Registry registry = materialized_registry();
    Z80Subject subject{"genesis materialized 3 images", gz80::executable_images(registry), {}, gz80::build_image_set(registry)};
    subject.kinds.assign(subject.set.images.size(), cz80::ImageKind::banked);
    z80_mutants(subject);
  }
  m68k_mutants();
  replacement_proof();
  determinism();
  std::printf("mutants: %d detected, %d asserted equivalent\n", detected, equivalent);
  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("executable_image_mutation_test: all checks passed\n");
  return 0;
}
