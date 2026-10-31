#include "segarecomp/machine/sega_cd/bios_profile.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <thread>

#include "segarecomp/cpu/m68k/decode.hpp"
#include "segarecomp/cpu/m68k/ir.hpp"
#include "segarecomp/cpu/m68k/timing.hpp"
#include "segarecomp/sha256.hpp"

#include "emit_shared.hpp"

namespace segarecomp::machine::sega_cd {
namespace {

using detail::hex;
using detail::prefetch_position;

std::uint32_t be32(std::span<const std::uint8_t> b, std::size_t o) {
  return (static_cast<std::uint32_t>(b[o]) << 24) | (static_cast<std::uint32_t>(b[o + 1]) << 16) | (static_cast<std::uint32_t>(b[o + 2]) << 8) |
         b[o + 3];
}

std::string bytes_hex(std::span<const std::uint8_t> bytes) {
  std::string out;
  for (const auto b : bytes) out += hex(b, 2);
  return out;
}

std::string c_string(const std::string &text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') out.push_back('\\');
    if (static_cast<unsigned char>(c) < 0x20U || static_cast<unsigned char>(c) >= 0x7FU) {
      out += "\\" + std::to_string((static_cast<unsigned char>(c) >> 6) & 7U) + std::to_string((static_cast<unsigned char>(c) >> 3) & 7U) +
             std::to_string(static_cast<unsigned char>(c) & 7U);
    } else {
      out.push_back(c);
    }
  }
  return out + "\"";
}

bool main_ram(std::uint32_t address) { return address >= 0x00E00000U && address <= 0x00FFFFFFU; }
bool ram_of(Processor p, std::uint32_t address, std::uint32_t length) {
  if (length == 0U) return false;
  const std::uint64_t end = static_cast<std::uint64_t>(address) + length;
  if (p == Processor::main) return main_ram(address) && end <= 0x01000000U;
  return end <= kPrgRamBytes;
}
// SEG-071-T001: a disc extent may also be placed in the main processor's 2M Word RAM view ($200000-$23FFFF, contract section 15): code
// the sub loads there from the disc and main executes in place. The extent lies wholly inside the view; whether main may fetch it (owner,
// 1M/2M mode) and whether its bytes are live is decided per instruction at run time (the live view of scd_code_byte), never here.
bool main_word_ram(std::uint32_t address, std::uint32_t length) {
  const std::uint64_t end = static_cast<std::uint64_t>(address) + length;
  return length != 0U && address >= 0x00200000U && end <= 0x00240000U;
}
// The sub's linear 2M Word RAM view ($080000-$0BFFFF): code the sub loads from the disc and executes in place (Earthworm Jim SE level
// modules). Live ownership (2M, sub-owned) is decided per instruction at run time by scd_code_byte, exactly as for the main view.
bool sub_word_ram(std::uint32_t address, std::uint32_t length) {
  const std::uint64_t end = static_cast<std::uint64_t>(address) + length;
  return length != 0U && address >= 0x00080000U && end <= 0x000C0000U;
}
bool disc_extent_ram_of(Processor p, std::uint32_t address, std::uint32_t length) {
  return ram_of(p, address, length) || (p == Processor::main && main_word_ram(address, length)) ||
         (p == Processor::sub && sub_word_ram(address, length));
}

// A decoded operand template: one JMP (xxx).W/.L at its address.
struct Template {
  Processor processor{};
  std::uint32_t address{};
  std::vector<std::uint8_t> raw;  // the instruction's bytes as observed (opcode word guarded; operand live)
  bool long_operand = false;
  std::string timing;
};

std::optional<Template> decode_template(const Admission &a) {
  if (a.bytes.size() < 6U || (a.address & 1U) != 0U || !ram_of(a.processor, a.address, 6U)) return std::nullopt;
  const DecodeSource source{CpuVariant::mc68000, M68kProgramAddress{TargetAddressSpace::m68k_program, a.address}, MoveqImageOffset{0}};
  const auto result = decode_m68k_instruction(std::span<const std::uint8_t>(a.bytes), source, M68kDecodeProfile::general_startup);
  const auto *d = std::get_if<M68kDecodedInstruction>(&result);
  if (d == nullptr || d->kind != M68kInstructionKind::jmp) return std::nullopt;
  if (d->source_ea.mode != M68kEaMode::absolute_word && d->source_ea.mode != M68kEaMode::absolute_long) return std::nullopt;
  const auto op = lift_m68k_instruction(*d);
  const auto timing = m68k_timing_c_expression(op);
  if (!timing) return std::nullopt;
  Template t;
  t.processor = a.processor;
  t.address = a.address;
  t.raw = d->raw_bytes;
  t.long_operand = d->source_ea.mode == M68kEaMode::absolute_long;
  t.timing = *timing;
  return t;
}

struct Image {
  Processor processor{};
  std::string name;
  std::string extent;  // SEG-064-T004: a disc image's coordinates (provenance evidence)
  ImageAuthority authority{};
  bool guarded = false;
  std::uint32_t base{};
  std::vector<std::uint8_t> bytes;
  bool epoch = false;  // verified whole at the sub runnable epoch (materialized by a pass, or SEG-079 reconstructed from a recipe)
  std::string role = std::string();      // SEG-097 follow-up: the provenance that decides the selective policy (bios_firmware, bios_derived_copy, bios_derived_epoch, manual_disc_image, boot_area_extent, disc_extent)
  bool m3_eligible = false;  // only an independently admitted game extent outside the disc's boot area
};

struct Variant {
  std::size_t image{};
  std::string name;
  std::vector<std::uint8_t> raw;
  bool guarded = false;
  detail::LoweredInstruction lowered;
  std::uint32_t prefetch{};
};

std::string identity_text(const DiscImage &disc, const BiosProfileInputs &inputs, std::size_t admissions) {
  std::ostringstream t;
  t << kSegaCdBiosGeneratorVersion << "\ndisc " << disc.identity() << "\nbios " << inputs.bios.sha256 << "\n";
  if (!inputs.aot_policy.empty()) t << "aot experimental_m3 " << inputs.aot_policy << "\n";  // absent for Compatibility: its identity is unchanged
  for (const auto &c : inputs.copies)
    t << "copy " << processor_name(c.processor) << " " << c.base << " " << c.rom_offset << " " << c.length << "\n";
  for (const auto &d : inputs.disc_images)  // SEG-064-T004: the extent's coordinates and a digest of its bytes (a different disc is a different program)
    t << "discimage " << processor_name(d.processor) << " " << d.track_offset << " " << d.base << " " << d.length << " " << sha256_hex(d.bytes)
      << "\n";
  if (!inputs.dynamic_images.empty()) t << "dynamic " << dynamic_images_identity(inputs.dynamic_images, inputs.dynamic_policy) << "\n";   // SEG-100: absent without dynamic images
  if (inputs.application_entry != 0U) t << "entry " << inputs.application_entry << "\n";
  for (std::size_t i = 0; i < admissions && i < inputs.admissions.size(); ++i) {
    const auto &a = inputs.admissions[i];
    if (a.kind == AdmissionKind::epoch_recipe)  // SEG-079: no pass identity; the recipe and the BIOS identity above determine the bytes
      t << "epochrecipe " << epoch_recipe_text(a.recipe) << "\n";
    else if (a.kind == AdmissionKind::static_disc_extent)  // SEG-098-T002: the same coordinates and digest; the source proof is the static scan, not a pass
      t << "staticextent " << processor_name(a.processor) << " " << a.track_offset << " " << a.address << " " << a.bytes.size() << " " << sha256_hex(a.bytes) << "\n";
    else if (a.kind == AdmissionKind::disc_extent)  // SEG-078-T003: coordinates and a digest of the bytes, like a manual extent, in chain order
      t << "discextent " << a.pass_identity << " " << processor_name(a.processor) << " " << a.track_offset << " " << a.address << " " << a.bytes.size() << " "
        << sha256_hex(a.bytes) << "\n";
    else if (a.kind == AdmissionKind::static_fragment)  // SEG-112: no pass; the generation-time proof is recomputed by the caller from the disc
      t << "staticfragment " << processor_name(a.processor) << " " << a.address << " " << a.bytes.size() << " " << sha256_hex(a.bytes) << "\n";
    else if (a.kind == AdmissionKind::produced_image)  // SEG-111: processor, base and a digest of the produced bytes, in chain order
      t << "produced " << a.pass_identity << " " << processor_name(a.processor) << " " << a.address << " " << a.bytes.size() << " " << sha256_hex(a.bytes) << "\n";
    else if (a.kind == AdmissionKind::epoch)
      t << "epoch " << a.pass_identity << " " << a.bytes.size() << " " << sha256_hex(a.bytes) << "\n";
    else
      t << (a.slots > 1U ? "templaterun " : "template ") << a.pass_identity << " " << processor_name(a.processor) << " " << a.address << " "
        << (a.slots > 1U ? std::to_string(a.slots) + " " : std::string()) << bytes_hex(a.bytes) << "\n";
  }
  return t.str();
}

}  // namespace

const char *bios_error_name(BiosError error) noexcept {
  switch (error) {
  case BiosError::none: return "none";
  case BiosError::size_invalid: return "bios_size_invalid";
  case BiosError::header_invalid: return "bios_header_invalid";
  case BiosError::reset_vector_invalid: return "bios_reset_vector_invalid";
  }
  return "unknown";
}

const char *bios_profile_error_name(BiosProfileError error) noexcept {
  switch (error) {
  case BiosProfileError::none: return "none";
  case BiosProfileError::copy_invalid: return "copy_invalid";
  case BiosProfileError::disc_image_invalid: return "disc_image_invalid";
  case BiosProfileError::provenance_mismatch: return "provenance_mismatch";
  case BiosProfileError::epoch_invalid: return "epoch_invalid";
  case BiosProfileError::epoch_recipe_invalid: return "epoch_recipe_invalid";
  case BiosProfileError::static_fragment_invalid: return "static_fragment_invalid";
  case BiosProfileError::produced_invalid: return "produced_invalid";
  case BiosProfileError::template_invalid: return "template_invalid";
  case BiosProfileError::image_validation: return "image_validation";
  case BiosProfileError::partition_invalid: return "partition_invalid";
  case BiosProfileError::dynamic_invalid: return "dynamic_invalid";
  }
  return "unknown";
}

BiosAdmission admit_bios(std::span<const std::uint8_t> bytes) {
  BiosAdmission out;
  if (bytes.size() < kBiosMinimumBytes || bytes.size() > kBiosWindowBytes || (bytes.size() & 1U) != 0U) {
    out.error = BiosError::size_invalid;
    return out;
  }
  if (bytes[0x100] != 'S' || bytes[0x101] != 'E' || bytes[0x102] != 'G' || bytes[0x103] != 'A') {
    out.error = BiosError::header_invalid;
    return out;
  }
  BiosImage image;
  image.reset_ssp = be32(bytes, 0);
  image.reset_pc = be32(bytes, 4) & 0x00FFFFFFU;
  if ((image.reset_ssp & 1U) != 0U || (image.reset_pc & 1U) != 0U || image.reset_pc < kBiosMinimumBytes ||
      image.reset_pc >= bytes.size()) {
    out.error = BiosError::reset_vector_invalid;
    return out;
  }
  image.bytes.assign(bytes.begin(), bytes.end());
  image.sha256 = sha256_hex(bytes);
  out.bios = std::move(image);
  return out;
}

std::string bios_profile_identity(const DiscImage &disc, const BiosProfileInputs &inputs, std::size_t admissions) {
  const auto text = identity_text(disc, inputs, admissions);
  return sha256_hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(text.data()), text.size()));
}

bool valid_partition_window(std::uint32_t window) noexcept {
  return window >= kMinPartitionWindow && window <= kMaxPartitionWindow && (window & (window - 1U)) == 0U;
}

namespace {

// What the selective pass hands the emission loop: the validated proposal of each image (by index into the emission's image list).
struct SelectiveState {
  const SelectiveAdmissionRequest *request = nullptr;  // non-null: apply the producer's proposals
  SelectiveAdmissionOutcome outcome;
  std::map<std::size_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>> ranges;
  std::map<std::size_t, std::set<std::uint32_t>> admitted;  // distinct source offsets compiled, by image index (always tracked)
};

BiosProfileEmission emit_bios_once(const DiscImage &disc, const BiosProfileInputs &inputs, const BiosProfileEmitOptions &options, SelectiveState &selective);

}  // namespace

BiosProfileEmission emit_sega_cd_bios_program(const DiscImage &disc, const BiosProfileInputs &inputs, const BiosProfileEmitOptions &options) {
  if (options.selective == nullptr || !options.selective->propose) {
    SelectiveState none;
    return emit_bios_once(disc, inputs, options, none);
  }
  // Experimental selective admission (ADR 0208): the selective emission first; on any failure (or an emission error) the broad emission, and the reason.
  SelectiveState attempt;
  attempt.request = options.selective;
  auto selective = emit_bios_once(disc, inputs, options, attempt);
  SelectiveState broad_state;
  auto broad = emit_bios_once(disc, inputs, options, broad_state);
  auto &outcome = attempt.outcome;
  outcome.requested = true;
  if (selective.error != BiosProfileError::none && outcome.reason == "none") outcome.reason = "validator_rejected:emission_" + std::string(bios_profile_error_name(selective.error));
  const bool selective_ok = outcome.admitted && selective.error == BiosProfileError::none;
  // Per-image counts. Images that are not scored are broad in either emission; scored images compare the selective set with the broad one (unknown, 0, if the
  // reference emission failed). A fallback build admits every entry.
  const auto tally = [&](bool applied) {
    outcome.broad_entries = outcome.admitted_entries = outcome.m3_images = outcome.m3_broad_entries = outcome.m3_admitted_entries = 0;
    std::vector<std::pair<std::string, std::vector<std::uint32_t>>> rows;
    for (std::size_t i = 0; i < outcome.images.size(); ++i) {
      auto &r = outcome.images[i];
      const std::size_t chosen = applied ? attempt.admitted[i].size() : broad_state.admitted[i].size();
      r.broad_entries = static_cast<std::uint32_t>(broad.error == BiosProfileError::none ? broad_state.admitted[i].size() : attempt.admitted[i].size());
      r.admitted_entries = static_cast<std::uint32_t>(applied && r.selection == ImageSelection::m3_selected ? chosen : r.broad_entries);
      outcome.broad_entries += r.broad_entries;
      outcome.admitted_entries += r.admitted_entries;
      if (applied && r.selection == ImageSelection::m3_selected) {
        ++outcome.m3_images;
        outcome.m3_broad_entries += r.broad_entries;
        outcome.m3_admitted_entries += r.admitted_entries;
      }
      const auto &set = applied ? attempt.admitted[i] : broad_state.admitted[i];
      rows.emplace_back(std::string(processor_name(r.processor)) + ":" + r.name, std::vector<std::uint32_t>(set.begin(), set.end()));
    }
    return rows;
  };
  if (selective_ok) {
    auto rows = tally(true);
    if (outcome.m3_images == 0U) {
      outcome.reason = "no_eligible_images";
    } else if (outcome.m3_admitted_entries == 0U && outcome.m3_broad_entries != 0U) {
      outcome.reason = "validator_rejected:nothing_admitted";
    } else {
      outcome.k_sha256 = admitted_set_digest(rows);
      selective.admission = outcome;
      return selective;   // a valid selective result is not discarded because the comparison emission failed
    }
  }
  outcome.admitted = false;
  if (outcome.reason == "none") outcome.reason = "validator_rejected:unclassified";
  for (auto &r : outcome.images)
    if (r.selection == ImageSelection::m3_selected) r.selection = ImageSelection::broad_fallback;
  if (broad.error == BiosProfileError::none) tally(false);
  outcome.k_sha256.clear();
  broad.admission = outcome;
  return broad;
}

namespace {

BiosProfileEmission emit_bios_once(const DiscImage &disc, const BiosProfileInputs &inputs, const BiosProfileEmitOptions &options, SelectiveState &selective) {
  BiosProfileEmission result;
  const auto fail = [&](BiosProfileError error, std::size_t index) {
    result.error = error;
    result.error_index = static_cast<std::uint32_t>(index);
    return result;
  };
  const auto &rom = inputs.bios.bytes;

  // ---- images, in a fixed order: bios, copies, epochs ----
  std::vector<Image> images;
  images.push_back(Image{Processor::main, "bios", std::string(), ImageAuthority::immutable_input, false, 0U, rom});
  images.back().role = "bios_firmware";
  if (inputs.copies.size() > kMaxCopyImages) return fail(BiosProfileError::copy_invalid, kMaxCopyImages);
  for (std::size_t i = 0; i < inputs.copies.size(); ++i) {
    const auto &c = inputs.copies[i];
    if (((c.base | c.rom_offset | c.length) & 1U) != 0U || static_cast<std::uint64_t>(c.rom_offset) + c.length > rom.size() ||
        !ram_of(c.processor, c.base, c.length))
      return fail(BiosProfileError::copy_invalid, i);
    images.push_back(Image{c.processor, "copy" + std::to_string(i), std::string(), ImageAuthority::static_proof, true, c.base,
                           std::vector<std::uint8_t>(rom.begin() + c.rom_offset, rom.begin() + c.rom_offset + c.length)});
    images.back().role = "bios_derived_copy";
  }
  if (inputs.disc_images.size() > kMaxDiscImages) return fail(BiosProfileError::disc_image_invalid, kMaxDiscImages);
  for (std::size_t i = 0; i < inputs.disc_images.size(); ++i) {
    const auto &d = inputs.disc_images[i];
    if (((d.base | d.length) & 1U) != 0U || d.length == 0U || d.bytes.size() != d.length || !disc_extent_ram_of(d.processor, d.base, d.length))
      return fail(BiosProfileError::disc_image_invalid, i);
    images.push_back(Image{d.processor, "disc" + std::to_string(i), "track:1 byte:" + std::to_string(d.track_offset) + " length:" + std::to_string(d.length),
                           ImageAuthority::immutable_input, true, d.base, d.bytes});
    images.back().role = "manual_disc_image";   // a user-declared extent: its role (boot code or game code) is not established, so it stays broad
  }
  std::vector<Template> templates;
  std::uint32_t epochs = 0;
  std::size_t disc_extents = 0, produced = 0, static_fragments = 0;
  for (std::size_t i = 0; i < inputs.admissions.size(); ++i) {
    const auto &a = inputs.admissions[i];
    if (a.kind == AdmissionKind::epoch_recipe) {
      // SEG-079 (ADR 0179): a static producer. The image is derived from the immutable BIOS image alone and is verified whole at the epoch.
      EpochRecipeError recipe_error{};
      auto bytes = reconstruct_epoch_image(rom, a.recipe, &recipe_error);
      if (!bytes || ++epochs > kMaxEpochImages) return fail(BiosProfileError::epoch_recipe_invalid, i);
      Image image{Processor::sub, "epoch" + std::to_string(epochs - 1U), epoch_recipe_text(a.recipe), ImageAuthority::static_proof, true,
                  0U, std::move(*bytes)};
      image.epoch = true;
      image.role = "bios_derived_epoch";
      images.push_back(std::move(image));
      continue;
    }
    const bool is_static = a.kind == AdmissionKind::static_disc_extent || a.kind == AdmissionKind::static_fragment;
    if (!is_static && a.pass_identity != bios_profile_identity(disc, inputs, i)) return fail(BiosProfileError::provenance_mismatch, i);
    if (a.kind == AdmissionKind::static_fragment) {
      // SEG-112 (ADR 0256): bytes proven at generation time from immutable boot images and MC68000 semantics; static_proof authority, byte_identity verification.
      if (++static_fragments > kMaxStaticFragments || a.bytes.empty() || a.bytes.size() > kMaxProducedBytes || ((a.address | a.bytes.size()) & 1U) != 0U ||
          !ram_of(a.processor, a.address, static_cast<std::uint32_t>(a.bytes.size())))
        return fail(BiosProfileError::static_fragment_invalid, i);
      images.push_back(Image{a.processor, "fragment" + std::to_string(static_fragments - 1U), "bytes:" + std::to_string(a.bytes.size()), ImageAuthority::static_proof, true, a.address, a.bytes});
      images.back().role = "static_fragment";
      continue;
    }
    if (a.kind == AdmissionKind::produced_image) {
      // SEG-111 (ADR 0255): bytes computed by verified guest code in a build pass, bounded to main Work RAM / sub PRG RAM (no Word RAM, no disc offset). The
      // authority is the epoch image's: bounded build-time materialization verified by byte identity at run time.
      if (++produced > kMaxProducedImages || a.bytes.empty() || a.bytes.size() > kMaxProducedBytes || ((a.address | a.bytes.size()) & 1U) != 0U ||
          !ram_of(a.processor, a.address, static_cast<std::uint32_t>(a.bytes.size())))
        return fail(BiosProfileError::produced_invalid, i);
      images.push_back(Image{a.processor, "produced" + std::to_string(produced - 1U), "bytes:" + std::to_string(a.bytes.size()),
                             ImageAuthority::bounded_build_time_materialization, true, a.address, a.bytes});
      images.back().role = "produced_image";
      continue;
    }
    if (a.kind == AdmissionKind::disc_extent || is_static) {
      // SEG-078-T003 (ADR 0205): a discovered extent is a disc image whose position in the chain records the pass that explained it. The bytes were read
      // from the verified disc by the caller; the same limits as a manual extent apply, to both kinds together.
      const auto taken = inputs.disc_images.size() + ++disc_extents;
      if (taken > kMaxDiscImages || ((a.address | a.bytes.size()) & 1U) != 0U || a.bytes.empty() || !disc_extent_ram_of(a.processor, a.address, static_cast<std::uint32_t>(a.bytes.size())))
        return fail(BiosProfileError::disc_image_invalid, i);
      images.push_back(Image{a.processor, "disc" + std::to_string(taken - 1U), "track:1 byte:" + std::to_string(a.track_offset) + " length:" + std::to_string(a.bytes.size()),
                             ImageAuthority::immutable_input, true, a.address, a.bytes});
      // A discovered extent is a game module unless it lies in the disc's system area (the boot header, IP and SP live in its first 32 KiB): boot code stays broad.
      images.back().m3_eligible = a.track_offset >= kSegaCdSystemAreaBytes;
      images.back().role = images.back().m3_eligible ? "disc_extent" : "boot_area_extent";
      continue;
    }
    if (a.kind == AdmissionKind::epoch) {
      const auto n = a.bytes.size();
      if (n == 0U || n > kPrgRamBytes || n % kEpochGranule != 0U || ++epochs > kMaxEpochImages ||
          std::all_of(a.bytes.end() - kEpochGranule, a.bytes.end(), [](std::uint8_t b) { return b == 0U; }))
        return fail(BiosProfileError::epoch_invalid, i);
      images.push_back(Image{Processor::sub, "epoch" + std::to_string(epochs - 1U), std::string(), ImageAuthority::bounded_build_time_materialization, true,
                             0U, a.bytes, true});
      images.back().role = "bios_derived_epoch";
    } else {
      // SEG-079-T002 (ADR 0180): a record is one instruction or a bounded run of contiguous JMP (xxx).L slots. Every slot is decoded and
      // admitted exactly as a single record would be (own opcode-word guard, live operand, compiled-destination check); the run only
      // batches the admission. A run that does not re-validate slot by slot is refused whole.
      if (a.slots == 0U || a.slots > kMaxTemplateRunSlots || (a.slots > 1U && a.bytes.size() != std::size_t{a.slots} * kTemplateSlotBytes))
        return fail(BiosProfileError::template_invalid, i);
      for (std::uint32_t slot = 0; slot < a.slots; ++slot) {
        Admission one = a;
        one.slots = 1U;
        if (a.slots > 1U) {
          const auto first = a.bytes.begin() + static_cast<std::ptrdiff_t>(slot * kTemplateSlotBytes);
          one.address = a.address + slot * kTemplateSlotBytes;
          one.bytes.assign(first, first + kTemplateSlotBytes);
          if (one.bytes[0] != 0x4EU || one.bytes[1] != 0xF9U || (one.bytes[5] & 1U) != 0U)  // JMP (xxx).L with an even operand only
            return fail(BiosProfileError::template_invalid, i);
        }
        auto t = decode_template(one);
        if (!t || templates.size() >= kMaxOperandTemplates) return fail(BiosProfileError::template_invalid, i);
        for (const auto &other : templates)
          if (other.processor == t->processor && other.address == t->address) return fail(BiosProfileError::template_invalid, i);
        templates.push_back(std::move(*t));
      }
    }
  }

  // ---- the generic executable-image contract: one set per processor instance ----
  for (const Processor p : {Processor::main, Processor::sub}) {
    ExecutableImageSet set;
    set.cpu = CpuVariant::mc68000;
    set.platform_selector = true;  // overlapping RAM images are selected per instruction by live bytes (ADR 0142 decision 5)
    for (const auto &image : images) {
      if (image.processor != p) continue;
      ExecutableImage e;
      e.id = ImageId{static_cast<std::uint32_t>(set.images.size() + 1U)};
      e.bytes = image.bytes;
      e.mappings = {ImageMapping{image.base, 0U, static_cast<std::uint32_t>(image.bytes.size())}};
      e.provenance.authority = image.authority;
      e.provenance.producer = image.name == "bios" ? "sega_cd.bios_image"
                              : image.name.rfind("disc", 0) == 0 ? "sega_cd.disc_extent"
                              : image.name.rfind("produced", 0) == 0 ? "sega_cd.produced_image"
                              : image.name.rfind("fragment", 0) == 0 ? "sega_cd.static_fragment"
                              : image.epoch && image.authority == ImageAuthority::static_proof ? "sega_cd.sub_epoch_recipe"
                              : image.authority == ImageAuthority::static_proof ? "sega_cd.bios_copy"
                                                                                : "sega_cd.sub_epoch";
      if (!image.extent.empty()) e.provenance.evidence = {{image.epoch ? "epoch.recipe" : image.name.rfind("produced", 0) == 0 ? "produced.stream" : image.name.rfind("fragment", 0) == 0 ? "static.producer" : "disc.extent", image.extent}};
      e.verification = image.guarded ? ImageVerification::byte_identity : ImageVerification::none;
      set.images.push_back(std::move(e));
    }
    if (set.images.empty()) continue;
    const auto validation = validate_executable_image_set(set);
    if (!validation.ok()) {
      result.validation = validation.error;
      return fail(BiosProfileError::image_validation, 0);
    }
  }

  // ---- experimental selective admission (ADR 0208): one proposal per IMAGE, validated; any failure ends this attempt (the caller emits broad) ----
  const bool apply = selective.request != nullptr;
  if (apply) {
    auto &outcome = selective.outcome;
    const auto eligible = [&](const Image &image) { return image.m3_eligible || selective.request->all_images; };
    for (std::size_t index = 0; index < images.size(); ++index) {   // every image is reported, scored or not
      const auto &image = images[index];
      ImageAdmissionReport report;
      report.selection = eligible(image) ? ImageSelection::m3_selected : ImageSelection::broad_required;
      report.selection_reason = image.role.empty() ? std::string("unknown_provenance") : image.role;
      report.processor = image.processor;
      report.name = image.name;
      report.image_id = static_cast<std::uint32_t>(index + 1U);
      report.source_bytes = static_cast<std::uint32_t>(image.bytes.size());
      outcome.images.push_back(std::move(report));
    }
    for (std::size_t index = 0; index < images.size(); ++index) {
      const auto &image = images[index];
      if (!eligible(image)) continue;   // firmware, BIOS-derived and boot-area images: broad, never scored
      std::vector<std::uint32_t> seeds;
      if (index == 0U && inputs.bios.reset_pc < image.bytes.size()) seeds.push_back(inputs.bios.reset_pc);
      RegionProposal proposal = selective.request->propose(image.bytes, seeds);
      auto &report = outcome.images[index];
      report.units = proposal.units;
      report.selected = proposal.selected;
      report.seed_units = proposal.seed_units;
      for (const auto &[b, e] : proposal.ranges) report.proposed_bytes += e - b;
      const std::string error = validate_region_proposal(proposal, static_cast<std::uint32_t>(image.bytes.size()), seeds);
      if (!error.empty()) {
        outcome.reason = error;
        outcome.failed_image = std::string(processor_name(image.processor)) + ":" + image.name;
        return result;
      }
      selective.ranges[index] = std::move(proposal.ranges);
    }
  }

  // ---- broad admission and lowering (or, when `apply`, only the proposed aligned starts) ----
  std::map<std::uint32_t, std::vector<Variant>> variants[2];
  for (std::size_t index = 0; index < images.size(); ++index) {
    const auto &image = images[index];
    const unsigned cpu = image.processor == Processor::main ? 0U : 1U;
    const detail::SegaCdM68kRuntimeCEmitter emitter(cpu);
    const std::string lookup = "scd_p" + std::to_string(cpu) + "_lookup";
    BiosProfileImageStats stats;
    stats.processor = image.processor;
    stats.name = image.name;
    stats.authority = image_authority_name(image.authority);
    stats.start = image.base;
    stats.length = static_cast<std::uint32_t>(image.bytes.size());
    for (std::uint32_t offset = 0; offset + 2U <= image.bytes.size(); offset += 2U) {
      if (apply && selective.ranges.count(index) != 0U && !ranges_contain(selective.ranges[index], offset)) {  // outside the proposal: never decoded, never compiled
        ++stats.omitted;
        continue;
      }
      const std::uint32_t pc = image.base + offset;
      const DecodeSource source{CpuVariant::mc68000, M68kProgramAddress{TargetAddressSpace::m68k_program, pc}, MoveqImageOffset{offset}};
      const auto decoded = decode_m68k_instruction(std::span<const std::uint8_t>(image.bytes), source, M68kDecodeProfile::general_startup);
      const auto *d = std::get_if<M68kDecodedInstruction>(&decoded);
      bool ok = false;
      if (d != nullptr) {
        const auto op = lift_m68k_instruction(*d);
        const bool lifted = !(d->kind != M68kInstructionKind::moveq && op.kind == M68kIrKind::write_moveq);
        if (lifted) {
          auto lowered = detail::lower_instruction(op, pc, emitter, lookup);
          if (lowered.compiled) {
            auto &list = variants[cpu][pc];
            const bool duplicate = std::any_of(list.begin(), list.end(), [&](const Variant &v) { return v.raw == d->raw_bytes; });
            if (duplicate) ++result.deduplicated;
            else list.push_back(Variant{index, "scd_p" + std::to_string(cpu) + "_i" + std::to_string(index) + "_" + hex(pc, 6), d->raw_bytes,
                                        image.guarded, std::move(lowered), prefetch_position(*d, pc)});
            ok = true;
          }
        }
      }
      ++(ok ? stats.compiled : stats.rejected);
      if (ok) selective.admitted[index].insert(offset);
    }
    result.images.push_back(std::move(stats));
  }

  // ---- C source ----
  // Every function, template and selection chain belongs to the translation unit of its (processor, PC window); with window 0 there
  // is one unit per processor and the concatenation below is exactly the single translation unit of SEG-064-T002.
  const std::uint32_t window = options.partition_window;
  if (window != 0U && !valid_partition_window(window)) return fail(BiosProfileError::partition_invalid, window);
  // SEG-100 (ADR 0221): file-first dynamic images. Their bodies and maps are separate translation units, so a partitioned emission is required.
  const bool dynamic_present = !inputs.dynamic_images.empty();
  DynamicEmission dynamic;
  if (dynamic_present) {
    if (window == 0U) return fail(BiosProfileError::partition_invalid, 0U);
    unsigned jobs = options.jobs != 0U ? options.jobs : std::thread::hardware_concurrency();
    jobs = std::max(1U, std::min(jobs, 16U));
    dynamic = emit_dynamic_images(disc, inputs.dynamic_images, inputs.dynamic_policy, inputs.application_entry != 0U, jobs);
    if (dynamic.error != DynamicError::none) {
      result.dynamic = std::move(dynamic);
      return fail(BiosProfileError::dynamic_invalid, result.dynamic.error_index);
    }
  }
  struct UnitText {
    std::string variants, templates, chains, cases;
    std::uint32_t functions = 0;
  };
  std::map<std::pair<unsigned, std::uint32_t>, UnitText> units;
  const auto unit_of = [&](unsigned cpu, std::uint32_t pc) -> UnitText & { return units[{cpu, window == 0U ? 0U : pc / window}]; };
  const auto unit_name = [&](unsigned cpu, std::uint32_t index) { return "scd_p" + std::to_string(cpu) + "_w" + hex(index * window, 6); };
  std::ostringstream out;
  result.program_identity = bios_profile_identity(disc, inputs, inputs.admissions.size());
  out << "/* Generated by segarecomp (" << kSegaCdBiosGeneratorVersion << ", SEG-064-T002, ADR 0142). Sega CD BIOS-image profile:\n"
      << " * verified broad AOT with bounded executable-image materialization. Program identity " << result.program_identity << ".\n"
      << " * Built from the supplied BIOS image: when that is a real BIOS this file embeds BIOS-derived code - keep it local, never\n"
      << " * publish it. Do not edit. */\n#include <stddef.h>\n#include <stdint.h>\n#include \"scd_runtime.h\"\n\n";
  for (unsigned cpu = 0; cpu < 2U; ++cpu) {
    std::vector<std::uint32_t> entries;
    for (const auto &[pc, list] : variants[cpu]) entries.push_back(pc);
    for (const auto &t : templates)
      if ((t.processor == Processor::main ? 0U : 1U) == cpu) entries.push_back(t.address);
    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
    result.entries[cpu] = static_cast<std::uint32_t>(entries.size());
    const std::string p = "scd_p" + std::to_string(cpu);
    out << "static const uint32_t " << p << "_entries[" << std::max<std::size_t>(entries.size(), 1U) << "] = {";
    for (std::size_t i = 0; i < entries.size(); ++i) out << (i ? ", " : "") << "UINT32_C(0x" << hex(entries[i]) << ")";
    if (entries.empty()) out << "0U";
    if (window != 0U)  // the units' computed control resolves through this shared lookup
      out << "};\nconst uint32_t *" << p << "_lookup(uint32_t pc);\nconst uint32_t *" << p << "_lookup(uint32_t pc) {\n";
    else
      out << "};\nstatic const uint32_t *" << p << "_lookup(uint32_t pc) {\n";
    out << "  size_t lo = 0U, hi = " << entries.size() << "U;\n  pc &= UINT32_C(0x00FFFFFF); /* the 68000 address bus is 24 bits (SEG-064-T004) */\n"
        << "  while (lo < hi) { const size_t mid = lo + (hi - lo) / 2U; if (" << p << "_entries[mid] == pc) return &" << p
        << "_entries[mid]; if (" << p << "_entries[mid] < pc) lo = mid + 1U; else hi = mid; }\n  return "
        << (dynamic_present ? "scd_dynamic_lookup(" + std::to_string(cpu) + "U, pc)" : std::string("NULL")) << ";\n}\n";
  }
  for (unsigned cpu = 0; cpu < 2U; ++cpu) {
    const std::string k = std::to_string(cpu) + "U";
    const std::string p = "scd_p" + std::to_string(cpu);
    std::map<std::uint32_t, const Template *> cpu_templates;
    for (const auto &t : templates)
      if ((t.processor == Processor::main ? 0U : 1U) == cpu) cpu_templates[t.address] = &t;
    for (const auto &[pc, list] : variants[cpu]) {
      auto &u = unit_of(cpu, pc);
      std::ostringstream functions;
      for (const auto &v : list) {
        if (v.guarded) {
          functions << "static const uint8_t " << v.name << "_bytes[] = {";
          for (std::size_t i = 0; i < v.raw.size(); ++i) functions << (i ? ", " : "") << "0x" << hex(v.raw[i], 2);
          functions << "};\n";
        }
        functions << "static int " << v.name << "(ScdMachine *m) {\n  ScdCpu *const cpu = &m->cpu[" << cpu << "];\n  uint32_t pc = cpu->pc;\n"
                  << "  cpu->prefetch = UINT32_C(0x" << hex(v.prefetch) << ");\n";
        if (v.guarded)
          functions << "  if (scd_code_guard(m, " << k << ", UINT32_C(0x" << hex(pc) << "), " << v.name << "_bytes, " << v.raw.size()
                    << "U) != 0) return 1;\n";
        // SEG-064-T004: with an application entry the same selection and byte guard can run without the instruction body (verify only)
        if (inputs.application_entry != 0U) functions << "  if (m->verify_only) return 0;\n";
        if (cpu == 0U) functions << v.lowered.refresh_skip;  // ADR 0177: this verified variant's own refresh-skip cycles
        functions << v.lowered.slots << v.lowered.body << "  cpu->pc = pc;\n  return scd_retire(m, " << k << ", (uint32_t)("
                  << v.lowered.timing << "));\n}\n";
        ++result.functions;
        ++u.functions;
      }
      u.variants += functions.str();
    }
    // Operand templates (ADR 0142 decision 7): the opcode word is guarded, the operand is read live with its fixed width, and the
    // destination must be an even compiled entry of this processor. Nothing is decoded at run time.
    for (const auto &[address, t] : cpu_templates) {
      auto &u = unit_of(cpu, address);
      std::ostringstream functions;
      const std::string name = p + "_t_" + hex(address, 6);
      const std::uint32_t operand = address + 2U;
      functions << "static const uint8_t " << name << "_opcode[] = {0x" << hex(t->raw[0], 2) << ", 0x" << hex(t->raw[1], 2) << "};\n"
                << "static int " << name << "(ScdMachine *m) {\n  ScdCpu *const cpu = &m->cpu[" << cpu << "];\n"
                << "  uint32_t target = UINT32_C(0);\n  cpu->prefetch = UINT32_C(0x" << hex(address + static_cast<std::uint32_t>(t->raw.size()))
                << ");\n"
                << "  if (scd_code_guard(m, " << k << ", UINT32_C(0x" << hex(address) << "), " << name << "_opcode, 2U) != 0) return 1;\n"
                << (inputs.application_entry != 0U ? "  if (m->verify_only) return 0;\n" : "")
                << "  if (scd_read(m, " << k << ", UINT32_C(0x" << hex(operand) << "), " << (t->long_operand ? "4U" : "2U")
                << ", &target, UINT32_C(0x" << hex(address) << ")) != 0) return 1;\n"
                << (t->long_operand ? "  target &= UINT32_C(0x00FFFFFF);\n"
                                    : "  target = (uint32_t)(int32_t)(int16_t)(uint16_t)target & UINT32_C(0x00FFFFFF);\n")
                << "  if ((target & 1U) != 0U || " << p << "_lookup(target) == NULL) return scd_fail(m, " << k
                << ", SCD_STOP_ENTRY_NOT_COMPILED, UINT32_C(0x" << hex(address) << "), target);\n"
                << "  cpu->pc = target;\n  return scd_retire(m, " << k << ", (uint32_t)(" << t->timing << "));\n}\n";
      u.templates += functions.str();
      ++result.functions;
      ++result.templates;
      ++u.functions;
    }
    // Per-PC selection: exact variants (each matches only when all of its bytes are live; distinct variants cannot both match), then
    // the PC's operand template, else code_bytes_mismatch. No priority among exact variants is needed.
    std::set<std::uint32_t> pcs;
    for (const auto &[pc, list] : variants[cpu]) pcs.insert(pc);
    for (const auto &[address, t] : cpu_templates) pcs.insert(address);
    for (const std::uint32_t pc : pcs) {
      auto &u = unit_of(cpu, pc);
      std::ostringstream cases, functions;
      const auto it = variants[cpu].find(pc);
      const std::size_t exact = it == variants[cpu].end() ? 0U : it->second.size();
      const bool has_template = cpu_templates.count(pc) != 0U;
      // SEG-100: with dynamic images a RAM PC is never a direct `case`: when its live bytes equal no static variant the chain offers the PC to the verified dynamic
      // bindings (a module loaded over an address an earlier extent compiled) before it fails with the typed code_bytes_mismatch. ROM-only PCs stay direct.
      bool any_guarded = false;
      if (it != variants[cpu].end())
        for (const auto &v : it->second) any_guarded = any_guarded || v.guarded;
      const bool dynamic_chain = dynamic_present && (any_guarded || has_template);
      if (exact == 1U && !has_template && !dynamic_chain) {
        u.cases += "  case UINT32_C(0x" + hex(pc) + "): return " + it->second.front().name + "(m);\n";
        continue;
      }
      if (exact == 0U && !dynamic_chain) {
        u.cases += "  case UINT32_C(0x" + hex(pc) + "): return " + p + "_t_" + hex(pc, 6) + "(m);\n";
        continue;
      }
      ++result.variant_pcs[cpu];
      const std::string chain = p + "_v_" + hex(pc, 6);
      functions << "static int " << chain << "(ScdMachine *m) {\n";
      static const std::vector<Variant> no_variants;
      for (const auto &v : (it != variants[cpu].end() ? it->second : no_variants)) {
        if (!v.guarded) {  // an unguarded (ROM) variant cannot overlap another image: the address is outside every RAM view
          functions << "  return " << v.name << "(m);\n";
          continue;
        }
        functions << "  if (scd_code_matches(m, " << k << ", UINT32_C(0x" << hex(pc) << "), " << v.name << "_bytes, " << v.raw.size()
                  << "U)) return " << v.name << "(m);\n";
      }
      if (has_template)
        functions << "  if (scd_code_matches(m, " << k << ", UINT32_C(0x" << hex(pc) << "), " << p << "_t_" << hex(pc, 6)
                  << "_opcode, 2U)) return " << p << "_t_" << hex(pc, 6) << "(m);\n";
      if (dynamic_present)
        functions << "  return scd_dynamic_dispatch(m, " << k << ", SCD_STOP_CODE_BYTES_MISMATCH);\n}\n";
      else
        functions << "  return scd_fail(m, " << k << ", SCD_STOP_CODE_BYTES_MISMATCH, UINT32_C(0x" << hex(pc) << "), UINT32_C(0x" << hex(pc)
                  << "));\n}\n";
      u.chains += functions.str();
      u.cases += "  case UINT32_C(0x" + hex(pc) + "): return " + chain + "(m);\n";
    }
  }
  const auto default_stop = [&](unsigned cpu) {
    const std::string k = std::to_string(cpu) + "U", c = std::to_string(cpu);
    if (dynamic_present)  // SEG-100: a PC no static case owns is offered to the verified dynamic bindings, which fail with the same typed stop
      return "  default: return scd_dynamic_dispatch(m, " + k + ", SCD_STOP_ENTRY_NOT_COMPILED);\n  }\n}\n";
    return "  default: return scd_fail(m, " + k + ", SCD_STOP_ENTRY_NOT_COMPILED, m->cpu[" + c + "].pc, m->cpu[" + c + "].pc);\n  }\n}\n";
  };
  if (window == 0U) {
    std::string dispatch;
    for (unsigned cpu = 0; cpu < 2U; ++cpu) {
      const auto it = units.find({cpu, 0U});
      const std::string p = "scd_p" + std::to_string(cpu);
      if (it != units.end()) out << it->second.variants << it->second.templates << it->second.chains;
      dispatch += "static int " + p + "_dispatch(ScdMachine *m) {\n  (void)" + p + "_lookup; /* referenced by computed control */\n" +
                  "  m->cpu[" + std::to_string(cpu) + "].pc &= UINT32_C(0x00FFFFFF); /* the 68000 address bus is 24 bits (SEG-064-T004) */\n" +
                  "  switch (m->cpu[" + std::to_string(cpu) + "].pc) {\n" + (it != units.end() ? it->second.cases : std::string()) +
                  default_stop(cpu);
    }
    out << dispatch;
  } else {
    for (const auto &[key, u] : units) {
      const auto [cpu, index] = key;
      const std::string name = unit_name(cpu, index), p = "scd_p" + std::to_string(cpu);
      BiosProfileUnit unit;
      unit.name = "p" + std::to_string(cpu) + "_" + hex(index * window, 6) + ".c";
      unit.cpu = cpu;
      unit.first_pc = index * window;
      unit.functions = u.functions;
      unit.c_source = "/* Generated by segarecomp (" + std::string(kSegaCdBiosGeneratorVersion) +
                      ", SEG-064-T003, ADR 0142). Sega CD BIOS-image profile translation unit:\n * processor " + std::to_string(cpu) +
                      ", PCs [0x" + hex(index * window, 6) + ", 0x" + hex(index * window + (window - 1U), 6) +
                      "]. Built from the supplied BIOS image: when that is a real BIOS this file embeds\n * BIOS-derived code - keep it "
                      "local, never publish it. Do not edit. */\n#include <stddef.h>\n#include <stdint.h>\n#include \"scd_runtime.h\"\n\n"
                      "const uint32_t *" + p + "_lookup(uint32_t pc);\nint " + name + "(ScdMachine *m);\n" + u.variants + u.templates +
                      u.chains + "int " + name + "(ScdMachine *m) {\n  switch (m->cpu[" + std::to_string(cpu) + "].pc) {\n" + u.cases +
                      default_stop(cpu);
      result.units.push_back(std::move(unit));
    }
    for (const auto &[key, u] : units) out << "int " << unit_name(key.first, key.second) << "(ScdMachine *m);\n";
    for (unsigned cpu = 0; cpu < 2U; ++cpu) {
      const std::string p = "scd_p" + std::to_string(cpu);
      out << "static int " << p << "_dispatch(ScdMachine *m) {\n  m->cpu[" << cpu << "].pc &= UINT32_C(0x00FFFFFF); /* 24-bit address bus */\n"
          << "  switch (m->cpu[" << cpu << "].pc / UINT32_C(0x" << hex(window) << ")) {\n";
      for (const auto &[key, u] : units)
        if (key.first == cpu) out << "  case UINT32_C(0x" << hex(key.second) << "): return " << unit_name(cpu, key.second) << "(m);\n";
      out << default_stop(cpu);
    }
  }
  if (dynamic_present) {  // SEG-100: the processor-neutral computed-control authority of the dynamic bodies, and the dynamic images' tables
    out << dynamic.program_text
        << "const uint32_t *scd_neutral_lookup(uint32_t pc);\nconst uint32_t *scd_neutral_lookup(uint32_t pc) {\n  return scd_dynamic_exec_cpu() == 0U ? scd_p0_lookup(pc) : scd_p1_lookup(pc);\n}\n";
  }
  out << "static const ScdDiscFile scd_disc_files[] = {\n";
  for (const auto &file : disc.files())
    out << "  {" << c_string(file.name) << ", UINT64_C(" << file.size << "), \"" << file.sha256 << "\"},\n";
  out << "};\nstatic const ScdDiscTrack scd_disc_tracks[] = {\n";
  for (const auto &track : disc.tracks())
    out << "  {" << track.number << "U, " << static_cast<unsigned>(track.type) << "U, " << track.file_index << "U, " << track.sector_size
        << "U, UINT64_C(" << track.file_offset << "), " << track.first_lba << "U, " << track.sector_count << "U},\n";
  out << "};\n";
  std::vector<const Image *> epoch_images;
  for (const auto &image : images)
    if (image.epoch) epoch_images.push_back(&image);
  if (!epoch_images.empty()) {
    out << "static const ScdMaterializedIdentity scd_sub_images[] = {\n";
    for (const auto *image : epoch_images) out << "  {" << image->bytes.size() << "U, \"" << sha256_hex(image->bytes) << "\"},\n";
    out << "};\n";
  }
  out << "static const ScdAuthentic scd_bios_profile = {\"" << inputs.bios.sha256 << "\", " << rom.size() << "U, "
      << (epoch_images.empty() ? "NULL" : "scd_sub_images") << ", " << epoch_images.size() << "U, UINT32_C(0x"
      << hex(inputs.application_entry) << ")};\n"
      << "static const ScdProgram scd_program = {\n  {{scd_p0_entries, " << result.entries[0] << "U, scd_p0_dispatch, UINT32_C(0x"
      << hex(inputs.bios.reset_pc) << "), 0U, 0U, 0U},\n   {scd_p1_entries, " << result.entries[1] << "U, scd_p1_dispatch, 0U, 0U, 0U, 0U}},\n"
      << "  {scd_disc_files, " << disc.files().size() << "U, scd_disc_tracks, " << disc.tracks().size() << "U, \"" << disc.identity()
      << "\", {0}},\n  UINT32_C(0x" << hex(inputs.bios.reset_ssp) << "),\n  \"" << result.program_identity << "\"" << "" << "\n};\n\n"
      << (inputs.main_z80 ? "extern const ScdZ80Ops scd_z80_ops;\n" : "")
      << "int main(int argc, char **argv) { " << (dynamic_present ? "scd_dynamic_register(&scd_dynamic_program); " : "")
      << (inputs.main_z80 ? "scd_z80_register(&scd_z80_ops); " : "") << "return scd_program_main_bios(&scd_program, &scd_bios_profile, argc, argv); }\n";
  if (dynamic_present) {
    for (auto &unit : dynamic.units) {
      BiosProfileUnit u;
      u.name = unit.name;
      u.cpu = 0U;
      u.first_pc = 0U;
      u.functions = unit.functions;
      u.c_source = std::move(unit.c_source);
      result.units.push_back(std::move(u));
    }
    dynamic.units.clear();
    result.dynamic = std::move(dynamic);
  }
  result.c_source = out.str();
  if (apply) selective.outcome.admitted = true;
  return result;
}

}  // namespace

}  // namespace segarecomp::machine::sega_cd
