// SEG-057-T002 (ADR 0119): the native SMS Z80 D512HP proposal producer. Hermetic: synthetic inputs only, no ROM, no PyTorch, no Python.
//
//   sms_ml_region_z80_test <parity-fixture.smsmlp>
//
// The fixture is produced once by tools/segarecomp_ml_region_z80_native_parity.py from the unmodified research model (PyTorch) on synthetic
// bytes. Registered tolerance (fixed before any parity number was looked at): |native - reference| <= 1e-4 + 1e-4 * |reference| per byte
// logit, and EXACT equality of the retained cell set at the frozen 65% budget.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "segarecomp/machine/master_system/emit.hpp"
#include "segarecomp/machine/master_system/image_set.hpp"
#include "segarecomp/machine/master_system/ml_region_z80_producer.hpp"

using namespace segarecomp::machine::master_system;
namespace z80 = segarecomp::codegen::z80;

namespace {

int failures = 0;
void check(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::printf("FAIL: %s\n", message.c_str());
  }
}

std::vector<std::uint8_t> read_file(const char *path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct Reader {
  const std::vector<std::uint8_t> &data;
  std::size_t pos = 0;
  std::uint32_t u32() {
    const std::uint32_t v = static_cast<std::uint32_t>(data.at(pos)) | static_cast<std::uint32_t>(data.at(pos + 1)) << 8U |
                            static_cast<std::uint32_t>(data.at(pos + 2)) << 16U | static_cast<std::uint32_t>(data.at(pos + 3)) << 24U;
    pos += 4;
    return v;
  }
  float f32() {
    const std::uint32_t bits = u32();
    float v;
    std::memcpy(&v, &bits, sizeof v);
    return v;
  }
};

std::uint32_t bits_of(float v) {
  std::uint32_t b;
  std::memcpy(&b, &v, sizeof b);
  return b;
}

void test_identity() {
  check(ml_identity_failure().empty(), "the embedded artifact must verify");
  const MlIdentity &id = ml_identity();
  check(id.artifact_sha256 == "81434de0da4755654e14c370fc32de7898532193cbc5a514033268ba6669c2ac", "artifact digest");
  check(id.ensemble_sha256 == "6bee77726a93ad33504522c35b84aaa6d89ed3c5327ff5fae6bff1e9558a4f11", "ensemble digest");
  check(ml_policy_identity().find(std::string(id.ensemble_sha256)) != std::string::npos, "policy identity carries the model identity");
  check(ml_policy_identity().find("budget65") != std::string::npos, "policy identity carries the rank policy");
}

void test_fail_closed_blobs(const char *artifact_path) {
  std::vector<std::uint8_t> blob = read_file(artifact_path);
  check(ml_verify_blob(blob).empty(), "the committed artifact verifies through the test seam");
  std::vector<std::uint8_t> flipped = blob;
  flipped[blob.size() / 2] ^= 1U;
  check(ml_verify_blob(flipped) == "model_identity", "a flipped weight byte must fail closed");
  std::vector<std::uint8_t> truncated(blob.begin(), blob.end() - 1);
  check(ml_verify_blob(truncated) == "model_identity", "a truncated artifact must fail closed");
  std::vector<std::uint8_t> extended = blob;
  extended.push_back(0);
  check(ml_verify_blob(extended) == "model_identity", "trailing bytes must fail closed");
  std::vector<std::uint8_t> bad_magic = blob;
  bad_magic[0] = 'X';
  check(ml_verify_blob(bad_magic) == "model_identity", "a bad magic must fail closed");
  check(ml_verify_blob({}) == "model_identity", "an empty artifact must fail closed");
}

void test_parity(const char *fixture_path) {
  const std::vector<std::uint8_t> file = read_file(fixture_path);
  check(file.size() > 1000, "fixture missing");
  if (file.size() <= 1000) return;
  check(std::memcmp(file.data(), "SMSMLP01", 8) == 0, "fixture magic");
  Reader r{file, 8};
  const std::uint32_t cases = r.u32();
  check(cases >= 7, "fixture case count");
  double worst = 0;
  std::size_t total = 0;
  for (std::uint32_t c = 0; c < cases; ++c) {
    const std::uint32_t n = r.u32();
    const std::vector<std::uint8_t> rom(file.begin() + static_cast<std::ptrdiff_t>(r.pos), file.begin() + static_cast<std::ptrdiff_t>(r.pos + n));
    r.pos += n;
    std::vector<float> reference(n);
    for (float &v : reference) v = r.f32();
    const std::uint32_t retained = r.u32();
    std::vector<std::uint32_t> cells(retained);
    for (std::uint32_t &cell : cells) cell = r.u32();
    const std::vector<float> logits = ml_byte_logits(rom, 1);
    check(logits.size() == n, "case " + std::to_string(c) + ": logit count");
    if (logits.size() != n) continue;
    for (std::uint32_t i = 0; i < n; ++i) {
      const double error = std::fabs(static_cast<double>(logits[i]) - static_cast<double>(reference[i]));
      worst = std::max(worst, error);
      ++total;
      check(error <= 1e-4 + 1e-4 * std::fabs(static_cast<double>(reference[i])),
            "case " + std::to_string(c) + " byte " + std::to_string(i) + ": native " + std::to_string(logits[i]) + " vs reference " +
                std::to_string(reference[i]));
      if (failures > 20) return;
    }
    check(ml_select_cells(ml_cell_scores(logits)) == cells, "case " + std::to_string(c) + ": the retained cell set must equal the reference exactly");
  }
  std::printf("parity: %zu logits over %u cases, worst absolute error %.3g (tolerance 1e-4 + 1e-4*|ref|)\n", total, cases, worst);
}

void test_determinism_and_chunking() {
  std::vector<std::uint8_t> rom(20011);  // not a multiple of 4 or 32; spans several 4096 windows
  std::uint64_t s = 99;
  for (auto &b : rom) {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    b = static_cast<std::uint8_t>(s >> 56);
  }
  const std::vector<float> base = ml_byte_logits(rom, 1);
  check(base.size() == rom.size(), "baseline logits");
  const auto same = [&](const std::vector<float> &other, const char *what) {
    bool equal = other.size() == base.size();
    for (std::size_t i = 0; equal && i < base.size(); ++i) equal = bits_of(other[i]) == bits_of(base[i]);
    check(equal, std::string(what) + " must be bit-identical");
  };
  same(ml_byte_logits(rom, 1), "a repeated run");
  same(ml_byte_logits(rom, 8), "an 8-thread run");
  same(ml_byte_logits(rom, 3, 1024), "central window 1024");
  same(ml_byte_logits(rom, 2, 64), "central window 64");
  same(ml_byte_logits(rom, 1, 20012), "a single window covering the image");
  check(ml_byte_logits(rom, 1, 6).empty(), "a central window that is not a multiple of 4 is rejected");
  check(ml_byte_logits({}, 1).empty(), "an empty image is rejected");
}

void test_ranking() {
  const std::vector<float> scores{1.0f, 3.0f, 3.0f, 2.0f, 3.0f, -1.0f};
  check(ml_rank_cells(scores) == (std::vector<std::uint32_t>{1, 2, 4, 3, 0, 5}), "ties break by the lower cell index");
  check(ml_rank_cells(std::vector<float>{1.0f, NAN}).empty(), "a NaN score fails closed");
  check(ml_rank_cells(std::vector<float>{1.0f, INFINITY}).empty(), "an infinite score fails closed");
  check(ml_cell_scores(std::vector<float>{1.0f, NAN}).empty(), "a NaN logit fails closed");
  check(ml_budget_cells(94, 65) == 62 && ml_budget_cells(1, 65) == 1 && ml_budget_cells(0, 65) == 0 && ml_budget_cells(10, 0) == 0 &&
            ml_budget_cells(10, 101) == 0 && ml_budget_cells(20, 65) == 13,
        "integer ceil(cells * percent / 100)");
  // top-two mean of a 32-byte cell, roots always retained
  std::vector<float> logits(64, 0.0f);
  logits[5] = 4.0f;
  logits[9] = 2.0f;
  logits[40] = 7.0f;
  const std::vector<float> cell_scores = ml_cell_scores(logits);
  check(cell_scores.size() == 2 && cell_scores[0] == 3.0f && cell_scores[1] == 3.5f, "top-two mean per 32-byte cell");
  std::vector<float> many(200, 0.0f);
  for (std::size_t i = 0; i < many.size(); ++i) many[i] = static_cast<float>(i);  // ranks the last cells first
  const std::vector<std::uint32_t> selected = ml_select_cells(many);
  check(std::binary_search(selected.begin(), selected.end(), 0U) && std::binary_search(selected.begin(), selected.end(), 1U) &&
            std::binary_search(selected.begin(), selected.end(), 3U),
        "the structural root cells 0, 1 and 3 are always retained");
  check(selected.size() == 130 + 3, "ceil(65%) of 200 cells plus the three root cells that the ranking did not pick");
  check(std::is_sorted(selected.begin(), selected.end()), "the retained set is sorted");
}

std::vector<std::uint8_t> synthetic_rom(std::size_t size, std::uint64_t seed) {
  std::vector<std::uint8_t> rom(size);
  for (auto &b : rom) {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    b = static_cast<std::uint8_t>(seed >> 56);
  }
  return rom;
}

void test_plan_mapping() {
  const std::vector<std::uint8_t> rom = synthetic_rom(0x10000, 7);  // 64 KiB: four 16 KiB banks
  IngestOptions options;
  options.explicit_profile = true;
  options.declarations.push_back({"sega", DeclarationSource::build_option});
  const IngestResult cartridge = ingest_cartridge(rom, options);
  check(cartridge.ok(), "synthetic cartridge ingests");
  const z80::ImageSet images = build_image_set(cartridge);
  const MlProposal proposal = ml_propose(cartridge, images, 4);
  check(proposal.ok, "proposal: " + proposal.failure);
  if (!proposal.ok) return;
  const std::vector<float> scores = ml_cell_scores(ml_byte_logits(cartridge.rom, 4));
  const std::vector<std::uint32_t> selected = ml_select_cells(scores);
  std::vector<char> keep(scores.size(), 0);
  for (const std::uint32_t c : selected) keep[c] = 1;
  check(proposal.cells == scores.size() && proposal.selected_cells == selected.size(), "reported cell counts");
  check(proposal.plan.images.size() == images.images.size(), "one plan entry per image");
  std::size_t total = 0;
  for (std::size_t i = 0; i < images.images.size(); ++i) {
    const z80::CodeImage &image = images.images[i];
    const z80::ImageAdmission &admission = proposal.plan.images[i];
    check(admission.identity == image.identity && admission.byte_count == image.bytes.size(), "plan entry is bound to its image");
    const std::size_t base = image.identity == 1 ? 0 : static_cast<std::size_t>(image.identity - 2) * 0x4000;
    std::vector<std::uint32_t> expected;
    for (std::size_t o = 0; o < image.bytes.size(); ++o)
      if (keep[(base + o) / 32]) expected.push_back(static_cast<std::uint32_t>(o));
    check(admission.offsets == expected, "image " + std::to_string(image.identity) + ": offsets are the retained ROM cells at the platform mapping");
    total += expected.size();
  }
  check(total == proposal.admitted_offsets, "admitted offset total");
  // roots: logical 0x0000 / 0x0038 / 0x0066 are fixed-image offsets and must be admitted in image 1
  const auto &fixed = proposal.plan.images.front().offsets;
  for (const std::uint32_t root : {0x0000U, 0x0038U, 0x0066U})
    check(std::binary_search(fixed.begin(), fixed.end(), root), "root " + std::to_string(root) + " admitted in the fixed image");
  // The plan is accepted by the existing emitter (structural validation) and the emission contains exactly the admitted starts.
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "sms_ml_region_z80_test_emit";
  std::filesystem::remove_all(dir);
  z80::EmitOptions emit;
  emit.directory = dir;
  emit.stem = "ml";
  emit.admission = proposal.plan;
  const z80::EmitResult emitted = z80::emit_image_set(images, emit);
  check(emitted.error.empty(), "the emitter accepts the proposed plan: " + emitted.error);
  check(emitted.stats.admitted_offsets == total, "emitter admitted-offset statistic");
  check(emitted.stats.entries <= total && emitted.stats.entries > total / 2, "entries follow the admitted starts (unlowered forms excepted)");
  std::filesystem::remove_all(dir);
  // A plan for other bytes is rejected by the emitter (identity / digest binding).
  std::vector<std::uint8_t> other = rom;
  other[0x5000] ^= 0xFFU;
  const IngestResult other_cartridge = ingest_cartridge(other, options);
  z80::EmitOptions wrong;
  wrong.directory = dir;
  wrong.admission = proposal.plan;
  check(!z80::emit_image_set(build_image_set(other_cartridge), wrong).error.empty(), "a plan made for different bytes is rejected");
  std::filesystem::remove_all(dir);
  // Failure classes
  IngestResult broken;
  broken.error = SMS_ERROR_MAPPER_UNDECLARED;
  check(ml_propose(broken, images).failure == "producer_input", "a rejected cartridge is a producer_input failure");
}

std::map<std::string, std::string> read_tree(const std::filesystem::path &dir) {
  std::map<std::string, std::string> files;
  if (!std::filesystem::exists(dir)) return files;
  for (const auto &entry : std::filesystem::directory_iterator(dir)) {
    std::ifstream in(entry.path(), std::ios::binary);
    files[entry.path().filename().string()] = std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  return files;
}

// Build-time failures of the Optimized request fall back to the ordinary broad AOT, byte for byte, with a classified machine-readable reason.
void test_build_fallbacks() {
  const std::vector<std::uint8_t> rom = synthetic_rom(0x8000, 11);  // 32 KiB: two banks
  IngestOptions options;
  options.explicit_profile = true;
  options.declarations.push_back({"sega", DeclarationSource::build_option});
  const std::filesystem::path base = std::filesystem::temp_directory_path() / "sms_ml_region_z80_test_fallback";
  std::filesystem::remove_all(base);
  const auto emit = [&](const char *name, bool optimized, std::function<MlProposal(const IngestResult &, const z80::ImageSet &)> proposer) {
    EmitRequest request;
    request.directory = base / name;
    request.stem = "sms";
    request.propose_admission = optimized;
    request.proposer = std::move(proposer);
    return emit_cartridge(rom, options, request);
  };
  const EmitOutcome broad = emit("broad", false, nullptr);
  check(broad.ok() && !broad.admission.requested && broad.admission.effective == "broad" && !broad.admission.fallback, "the Compatibility emission is the broad AOT");
  const auto broad_tree = read_tree(base / "broad");
  check(!broad_tree.empty(), "broad emission wrote files");

  const auto expect_fallback = [&](const char *name, const char *reason, std::function<MlProposal(const IngestResult &, const z80::ImageSet &)> proposer) {
    const EmitOutcome outcome = emit(name, true, std::move(proposer));
    check(outcome.ok(), std::string(name) + ": the emission must still succeed: " + outcome.error);
    check(outcome.admission.requested && outcome.admission.fallback && outcome.admission.effective == "broad" && outcome.admission.reason == reason &&
              outcome.admission.plan_sha256.empty() && outcome.admission.admitted_offsets == 0,
          std::string(name) + ": classified broad fallback '" + reason + "', got '" + outcome.admission.reason + "'");
    check(read_tree(base / name) == broad_tree, std::string(name) + ": the fallback program is byte-identical to the Compatibility program");
  };
  expect_fallback("model_identity", "model_identity", [](const IngestResult &, const z80::ImageSet &) { MlProposal p; p.failure = "model_identity"; return p; });
  expect_fallback("inference", "inference", [](const IngestResult &, const z80::ImageSet &) { MlProposal p; p.failure = "inference"; return p; });
  expect_fallback("input", "producer_input", [](const IngestResult &, const z80::ImageSet &) { MlProposal p; p.failure = "producer_input"; return p; });
  // A proposal the emitter rejects (made for other bytes): admission_rejected, and the failed emission left nothing behind.
  expect_fallback("rejected", "admission_rejected", [&](const IngestResult &cartridge, const z80::ImageSet &images) {
    MlProposal p = ml_propose(cartridge, images, 2);
    if (!p.ok) return p;
    p.plan.images.front().content_digest ^= 1U;
    return p;
  });
  expect_fallback("rejected_offset", "admission_rejected", [&](const IngestResult &cartridge, const z80::ImageSet &images) {
    MlProposal p = ml_propose(cartridge, images, 2);
    if (!p.ok) return p;
    p.plan.images.back().offsets.push_back(0xFFFFFFU);  // an offset outside its image
    return p;
  });
  // The native producer succeeds on this image: the Optimized emission is a strict subset, with its identity reported.
  const EmitOutcome real = emit("real", true, nullptr);
  check(real.ok() && real.admission.requested && !real.admission.fallback && real.admission.effective == "ml_region_z80" && real.admission.reason == "none",
        "the native producer's proposal is accepted: " + real.admission.reason);
  check(real.admission.plan_sha256.size() == 64 && real.admission.admitted_offsets > 0 && real.admission.admitted_offsets < real.admission.image_offsets &&
            real.stats.entries < broad.stats.entries,
        "the Optimized emission admits a strict subset and reports the plan identity");
  check(real.stats.admitted_offsets == real.admission.admitted_offsets, "the emitter and the report agree on the admitted count");
  const EmitOutcome again = emit("real_again", true, nullptr);
  check(again.ok() && again.admission.plan_sha256 == real.admission.plan_sha256 && read_tree(base / "real") == read_tree(base / "real_again"),
        "the Optimized emission is deterministic (plan identity and every generated byte)");
  // An explicit plan supplied by the caller wins; no producer runs.
  EmitRequest explicit_request;
  explicit_request.directory = base / "explicit";
  explicit_request.stem = "sms";
  explicit_request.propose_admission = true;
  explicit_request.codegen.admission = z80::AdmissionPlan{};
  explicit_request.proposer = [](const IngestResult &, const z80::ImageSet &) -> MlProposal { check(false, "the producer must not run when a plan is supplied"); return {}; };
  check(!emit_cartridge(rom, options, explicit_request).admission.requested, "a caller-supplied plan is not a producer request");
  std::filesystem::remove_all(base);
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::printf("usage: sms_ml_region_z80_test <parity-fixture.smsmlp> <weights.sgz80w>\n");
    return 2;
  }
  test_identity();
  test_fail_closed_blobs(argv[2]);
  test_ranking();
  test_parity(argv[1]);
  test_determinism_and_chunking();
  test_plan_mapping();
  test_build_fallbacks();
  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("sms_ml_region_z80: ok\n");
  return 0;
}
