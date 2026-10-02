#include "segarecomp/machine/master_system/emit.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

#include "segarecomp/codegen/c11/z80_executable_image.hpp"
#include "segarecomp/machine/master_system/image_set.hpp"

namespace segarecomp::machine::master_system {
namespace {

[[nodiscard]] std::string rom_source(const IngestResult& cartridge) {
  std::ostringstream out;
  out << "/* Embedded Master System cartridge (generated; ADR 0064 section 2). sha256 " << cartridge.identity.sha256 << " */\n"
      << "#include <stdint.h>\n\n"
      << "const uint32_t sms_rom_size = " << cartridge.rom.size() << "u;\n"
      << "const uint32_t sms_rom_mapper_family = " << static_cast<unsigned>(cartridge.identity.mapper) << "u; /* "
      << mapper_family_name(cartridge.identity.mapper) << " */\n"
      << "const uint8_t sms_rom_data[" << cartridge.rom.size() << "] = {";
  char buffer[8];
  for (std::size_t i = 0; i < cartridge.rom.size(); ++i) {
    if (i % 32 == 0) out << "\n";
    std::snprintf(buffer, sizeof buffer, "%u,", static_cast<unsigned>(cartridge.rom[i]));
    out << buffer;
  }
  out << "\n};\n";
  return out.str();
}

[[nodiscard]] bool write_file(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  return static_cast<bool>(out);
}

}  // namespace

std::string cartridge_provenance_json(const CartridgeIdentity& id) {
  std::ostringstream out;
  out << "{\n"
      << "  \"bank_count\": " << id.bank_count << ",\n"
      << "  \"checksum\": \"" << checksum_status_name(id.checksum) << "\",\n"
      << "  \"declaration_source\": \"" << declaration_source_name(id.declaration_source) << "\",\n"
      << "  \"header_class\": \"" << id.header_class << "\",\n"
      << "  \"header_offset\": " << id.header_offset << ",\n"
      << "  \"header_size_code\": " << id.header_size_code << ",\n"
      << "  \"mapper\": \"" << mapper_family_name(id.mapper) << "\",\n"
      << "  \"profile\": \"" << id.profile << "\",\n"
      << "  \"region\": \"" << id.region << "\",\n"
      << "  \"sha256\": \"" << id.sha256 << "\",\n"
      << "  \"size\": " << id.size << "\n"
      << "}\n";
  return out.str();
}

EmitOutcome emit_cartridge(std::span<const std::uint8_t> rom, const IngestOptions& options, const EmitRequest& request) {
  EmitOutcome outcome;
  const IngestResult cartridge = ingest_cartridge(rom, options);
  if (!cartridge.ok()) {
    outcome.sms_error = cartridge.error;
    outcome.error = std::string(sms_error_name(cartridge.error)) + ": " + cartridge.detail;
    return outcome;
  }
  outcome.identity = cartridge.identity;
  codegen::z80::EmitOptions codegen = request.codegen;
  codegen.directory = request.directory;
  codegen.stem = request.stem;
  // SEG-028 (ADR 0077): the producer's executable images, projected onto the emitter input; the provenance counts reported by the
  // build come from exactly these images.
  const ExecutableImageSet images = executable_images(cartridge);
  const codegen::z80::ImageProjection projected = codegen::z80::project_executable_images(images, image_kinds(images));
  if (!projected.ok()) {
    outcome.error = "executable image projection: " + projected.error;
    return outcome;
  }
  outcome.provenance = count_image_provenance(images);
  const codegen::z80::EmitResult emitted = codegen::z80::emit_image_set(projected.set, codegen);
  if (!emitted.error.empty()) {
    outcome.error = emitted.error;
    return outcome;
  }
  outcome.stats = emitted.stats;
  outcome.owners = emitted.owners;
  const auto rom_path = request.directory / (request.stem + "_rom.c");
  const auto json_path = request.directory / (request.stem + "_cartridge.json");
  const auto units_path = request.directory / (request.stem + ".units");
  std::string units;
  {
    std::ifstream in(units_path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    units = text.str();
  }
  if (!units.empty() && units.back() != '\n') units.push_back('\n');
  units += request.stem + "_rom.c\n";
  if (!write_file(rom_path, rom_source(cartridge)) || !write_file(json_path, cartridge_provenance_json(cartridge.identity)) ||
      !write_file(units_path, units)) {
    std::error_code ignored;
    std::filesystem::remove(rom_path, ignored);
    std::filesystem::remove(json_path, ignored);
    outcome.error = "cannot write the SMS cartridge outputs";
  }
  return outcome;
}

}  // namespace segarecomp::machine::master_system
