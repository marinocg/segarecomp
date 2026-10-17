#include "segarecomp/machine/sega_cd/bios_profile.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

#include "segarecomp/cpu/m68k/decode.hpp"
#include "segarecomp/cpu/m68k/ir.hpp"
#include "segarecomp/cpu/m68k/timing.hpp"
#include "segarecomp/sha256.hpp"

#include "emit_shared.hpp"

namespace segarecomp::machine::sega_cd {
namespace {

using detail::hex;

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
  ImageAuthority authority{};
  bool guarded = false;
  std::uint32_t base{};
  std::vector<std::uint8_t> bytes;
};

struct Variant {
  std::size_t image{};
  std::string name;
  std::vector<std::uint8_t> raw;
  bool guarded = false;
  detail::LoweredInstruction lowered;
};

std::string identity_text(const DiscImage &disc, const BiosProfileInputs &inputs, std::size_t admissions) {
  std::ostringstream t;
  t << kSegaCdBiosGeneratorVersion << "\ndisc " << disc.identity() << "\nbios " << inputs.bios.sha256 << "\n";
  for (const auto &c : inputs.copies)
    t << "copy " << processor_name(c.processor) << " " << c.base << " " << c.rom_offset << " " << c.length << "\n";
  for (std::size_t i = 0; i < admissions && i < inputs.admissions.size(); ++i) {
    const auto &a = inputs.admissions[i];
    if (a.kind == AdmissionKind::epoch)
      t << "epoch " << a.pass_identity << " " << a.bytes.size() << " " << sha256_hex(a.bytes) << "\n";
    else
      t << "template " << a.pass_identity << " " << processor_name(a.processor) << " " << a.address << " " << bytes_hex(a.bytes) << "\n";
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
  case BiosProfileError::provenance_mismatch: return "provenance_mismatch";
  case BiosProfileError::epoch_invalid: return "epoch_invalid";
  case BiosProfileError::template_invalid: return "template_invalid";
  case BiosProfileError::image_validation: return "image_validation";
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

BiosProfileEmission emit_sega_cd_bios_program(const DiscImage &disc, const BiosProfileInputs &inputs) {
  BiosProfileEmission result;
  const auto fail = [&](BiosProfileError error, std::size_t index) {
    result.error = error;
    result.error_index = static_cast<std::uint32_t>(index);
    return result;
  };
  const auto &rom = inputs.bios.bytes;

  // ---- images, in a fixed order: bios, copies, epochs ----
  std::vector<Image> images;
  images.push_back(Image{Processor::main, "bios", ImageAuthority::immutable_input, false, 0U, rom});
  if (inputs.copies.size() > kMaxCopyImages) return fail(BiosProfileError::copy_invalid, kMaxCopyImages);
  for (std::size_t i = 0; i < inputs.copies.size(); ++i) {
    const auto &c = inputs.copies[i];
    if (((c.base | c.rom_offset | c.length) & 1U) != 0U || static_cast<std::uint64_t>(c.rom_offset) + c.length > rom.size() ||
        !ram_of(c.processor, c.base, c.length))
      return fail(BiosProfileError::copy_invalid, i);
    images.push_back(Image{c.processor, "copy" + std::to_string(i), ImageAuthority::static_proof, true, c.base,
                           std::vector<std::uint8_t>(rom.begin() + c.rom_offset, rom.begin() + c.rom_offset + c.length)});
  }
  std::vector<Template> templates;
  std::uint32_t epochs = 0;
  for (std::size_t i = 0; i < inputs.admissions.size(); ++i) {
    const auto &a = inputs.admissions[i];
    if (a.pass_identity != bios_profile_identity(disc, inputs, i)) return fail(BiosProfileError::provenance_mismatch, i);
    if (a.kind == AdmissionKind::epoch) {
      const auto n = a.bytes.size();
      if (n == 0U || n > kPrgRamBytes || n % kEpochGranule != 0U || ++epochs > kMaxEpochImages ||
          std::all_of(a.bytes.end() - kEpochGranule, a.bytes.end(), [](std::uint8_t b) { return b == 0U; }))
        return fail(BiosProfileError::epoch_invalid, i);
      images.push_back(Image{Processor::sub, "epoch" + std::to_string(epochs - 1U), ImageAuthority::bounded_build_time_materialization, true,
                             0U, a.bytes});
    } else {
      auto t = decode_template(a);
      if (!t || templates.size() >= kMaxOperandTemplates) return fail(BiosProfileError::template_invalid, i);
      for (const auto &other : templates)
        if (other.processor == t->processor && other.address == t->address) return fail(BiosProfileError::template_invalid, i);
      templates.push_back(std::move(*t));
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
                              : image.authority == ImageAuthority::static_proof ? "sega_cd.bios_copy"
                                                                                : "sega_cd.sub_epoch";
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

  // ---- broad admission and lowering ----
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
                                        image.guarded, std::move(lowered)});
            ok = true;
          }
        }
      }
      ++(ok ? stats.compiled : stats.rejected);
    }
    result.images.push_back(std::move(stats));
  }

  // ---- C source ----
  std::ostringstream out, functions, dispatch;
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
    out << "};\nstatic const uint32_t *" << p << "_lookup(uint32_t pc) {\n  size_t lo = 0U, hi = " << entries.size() << "U;\n"
        << "  while (lo < hi) { const size_t mid = lo + (hi - lo) / 2U; if (" << p << "_entries[mid] == pc) return &" << p
        << "_entries[mid]; if (" << p << "_entries[mid] < pc) lo = mid + 1U; else hi = mid; }\n  return NULL;\n}\n";
  }
  for (unsigned cpu = 0; cpu < 2U; ++cpu) {
    const std::string k = std::to_string(cpu) + "U";
    const std::string p = "scd_p" + std::to_string(cpu);
    std::map<std::uint32_t, const Template *> cpu_templates;
    for (const auto &t : templates)
      if ((t.processor == Processor::main ? 0U : 1U) == cpu) cpu_templates[t.address] = &t;
    for (const auto &[pc, list] : variants[cpu]) {
      for (const auto &v : list) {
        if (v.guarded) {
          functions << "static const uint8_t " << v.name << "_bytes[] = {";
          for (std::size_t i = 0; i < v.raw.size(); ++i) functions << (i ? ", " : "") << "0x" << hex(v.raw[i], 2);
          functions << "};\n";
        }
        functions << "static int " << v.name << "(ScdMachine *m) {\n  ScdCpu *const cpu = &m->cpu[" << cpu << "];\n  uint32_t pc = cpu->pc;\n";
        if (v.guarded)
          functions << "  if (scd_code_guard(m, " << k << ", UINT32_C(0x" << hex(pc) << "), " << v.name << "_bytes, " << v.raw.size()
                    << "U) != 0) return 1;\n";
        functions << v.lowered.slots << v.lowered.body << "  cpu->pc = pc;\n  return scd_retire(m, " << k << ", (uint32_t)("
                  << v.lowered.timing << "));\n}\n";
        ++result.functions;
      }
    }
    // Operand templates (ADR 0142 decision 7): the opcode word is guarded, the operand is read live with its fixed width, and the
    // destination must be an even compiled entry of this processor. Nothing is decoded at run time.
    for (const auto &[address, t] : cpu_templates) {
      const std::string name = p + "_t_" + hex(address, 6);
      const std::uint32_t operand = address + 2U;
      functions << "static const uint8_t " << name << "_opcode[] = {0x" << hex(t->raw[0], 2) << ", 0x" << hex(t->raw[1], 2) << "};\n"
                << "static int " << name << "(ScdMachine *m) {\n  ScdCpu *const cpu = &m->cpu[" << cpu << "];\n"
                << "  uint32_t target = UINT32_C(0);\n"
                << "  if (scd_code_guard(m, " << k << ", UINT32_C(0x" << hex(address) << "), " << name << "_opcode, 2U) != 0) return 1;\n"
                << "  if (scd_read(m, " << k << ", UINT32_C(0x" << hex(operand) << "), " << (t->long_operand ? "4U" : "2U")
                << ", &target, UINT32_C(0x" << hex(address) << ")) != 0) return 1;\n"
                << (t->long_operand ? "  target &= UINT32_C(0x00FFFFFF);\n"
                                    : "  target = (uint32_t)(int32_t)(int16_t)(uint16_t)target & UINT32_C(0x00FFFFFF);\n")
                << "  if ((target & 1U) != 0U || " << p << "_lookup(target) == NULL) return scd_fail(m, " << k
                << ", SCD_STOP_ENTRY_NOT_COMPILED, UINT32_C(0x" << hex(address) << "), target);\n"
                << "  cpu->pc = target;\n  return scd_retire(m, " << k << ", (uint32_t)(" << t->timing << "));\n}\n";
      ++result.functions;
      ++result.templates;
    }
    // Per-PC selection: exact variants (each matches only when all of its bytes are live; distinct variants cannot both match), then
    // the PC's operand template, else code_bytes_mismatch. No priority among exact variants is needed.
    std::set<std::uint32_t> pcs;
    for (const auto &[pc, list] : variants[cpu]) pcs.insert(pc);
    for (const auto &[address, t] : cpu_templates) pcs.insert(address);
    std::ostringstream cases;
    for (const std::uint32_t pc : pcs) {
      const auto it = variants[cpu].find(pc);
      const std::size_t exact = it == variants[cpu].end() ? 0U : it->second.size();
      const bool has_template = cpu_templates.count(pc) != 0U;
      if (exact == 1U && !has_template) {
        cases << "  case UINT32_C(0x" << hex(pc) << "): return " << it->second.front().name << "(m);\n";
        continue;
      }
      if (exact == 0U) {
        cases << "  case UINT32_C(0x" << hex(pc) << "): return " << p << "_t_" << hex(pc, 6) << "(m);\n";
        continue;
      }
      ++result.variant_pcs[cpu];
      const std::string chain = p + "_v_" + hex(pc, 6);
      functions << "static int " << chain << "(ScdMachine *m) {\n";
      for (const auto &v : it->second) {
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
      functions << "  return scd_fail(m, " << k << ", SCD_STOP_CODE_BYTES_MISMATCH, UINT32_C(0x" << hex(pc) << "), UINT32_C(0x" << hex(pc)
                << "));\n}\n";
      cases << "  case UINT32_C(0x" << hex(pc) << "): return " << chain << "(m);\n";
    }
    dispatch << "static int " << p << "_dispatch(ScdMachine *m) {\n  (void)" << p << "_lookup; /* referenced by computed control */\n"
             << "  switch (m->cpu[" << cpu << "].pc) {\n" << cases.str() << "  default: return scd_fail(m, " << k
             << ", SCD_STOP_ENTRY_NOT_COMPILED, m->cpu[" << cpu << "].pc, m->cpu[" << cpu << "].pc);\n  }\n}\n";
  }
  out << functions.str() << dispatch.str();
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
    if (image.authority == ImageAuthority::bounded_build_time_materialization) epoch_images.push_back(&image);
  if (!epoch_images.empty()) {
    out << "static const ScdMaterializedIdentity scd_sub_images[] = {\n";
    for (const auto *image : epoch_images) out << "  {" << image->bytes.size() << "U, \"" << sha256_hex(image->bytes) << "\"},\n";
    out << "};\n";
  }
  out << "static const ScdAuthentic scd_bios_profile = {\"" << inputs.bios.sha256 << "\", " << rom.size() << "U, "
      << (epoch_images.empty() ? "NULL" : "scd_sub_images") << ", " << epoch_images.size() << "U};\n"
      << "static const ScdProgram scd_program = {\n  {{scd_p0_entries, " << result.entries[0] << "U, scd_p0_dispatch, UINT32_C(0x"
      << hex(inputs.bios.reset_pc) << "), 0U, 0U, 0U},\n   {scd_p1_entries, " << result.entries[1] << "U, scd_p1_dispatch, 0U, 0U, 0U, 0U}},\n"
      << "  {scd_disc_files, " << disc.files().size() << "U, scd_disc_tracks, " << disc.tracks().size() << "U, \"" << disc.identity()
      << "\", {0}},\n  UINT32_C(0x" << hex(inputs.bios.reset_ssp) << "),\n  \"" << result.program_identity << "\"\n};\n\n"
      << "int main(int argc, char **argv) { return scd_program_main_bios(&scd_program, &scd_bios_profile, argc, argv); }\n";
  result.c_source = out.str();
  return result;
}

}  // namespace segarecomp::machine::sega_cd
