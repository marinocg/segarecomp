#include "segarecomp/machine/genesis/z80_images.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "segarecomp/codegen/c11/z80_executable_image.hpp"
#include "segarecomp/sha256.hpp"

namespace segarecomp::machine::genesis::z80 {
namespace {

constexpr std::string_view kContentTag = "segarecomp.genesis.z80.image.v1";
constexpr std::string_view kSignatureTag = "segarecomp.genesis.z80.signature.v1.extents";

[[nodiscard]] Digest hash_bytes(const std::vector<std::uint8_t>& bytes) {
  const std::string hex = sha256_hex(bytes);
  Digest out{};
  for (std::size_t i = 0; i < out.size(); ++i) out[i] = static_cast<std::uint8_t>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
  return out;
}

void append_tag(std::vector<std::uint8_t>& bytes, std::string_view tag) { bytes.insert(bytes.end(), tag.begin(), tag.end()); }
void append_u16(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
  bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}
void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  append_u16(bytes, value & 0xFFFF);
  append_u16(bytes, value >> 16);
}

[[nodiscard]] bool write_file(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  return static_cast<bool>(out);
}

void c_digest(std::ostringstream& out, const Digest& digest) {
  out << "  {";
  char buffer[8];
  for (std::size_t i = 0; i < digest.size(); ++i) {
    std::snprintf(buffer, sizeof buffer, "%s0x%02X", i == 0 ? "" : ",", static_cast<unsigned>(digest[i]));
    out << buffer;
  }
  out << "},\n";
}

}  // namespace

std::vector<Extent> extents(std::span<const std::uint8_t, kBitmapBytes> written) {
  std::vector<Extent> runs;
  std::size_t start = 0;
  bool open = false;
  for (std::size_t offset = 0; offset < kRamBytes; ++offset) {
    const bool bit = ((written[offset >> 3] >> (offset & 7)) & 1) != 0;
    if (bit && !open) {
      start = offset;
      open = true;
    } else if (!bit && open) {
      runs.push_back({static_cast<std::uint16_t>(start), static_cast<std::uint16_t>(offset - start)});
      open = false;
    }
  }
  if (open) runs.push_back({static_cast<std::uint16_t>(start), static_cast<std::uint16_t>(kRamBytes - start)});
  return runs;
}

Digest content_digest(std::span<const std::uint8_t, kRamBytes> ram) {
  std::vector<std::uint8_t> bytes;
  append_tag(bytes, kContentTag);
  append_u32(bytes, static_cast<std::uint32_t>(kWindowBytes));
  bytes.push_back(static_cast<std::uint8_t>(kWindowBytes / kRamBytes));
  bytes.insert(bytes.end(), ram.begin(), ram.end());
  return hash_bytes(bytes);
}

Digest signature_digest(const Epoch& epoch) {
  std::vector<std::uint8_t> bytes;
  append_tag(bytes, kSignatureTag);
  for (const Extent& run : extents(epoch.written)) {
    append_u16(bytes, run.offset);
    append_u16(bytes, run.length);
    bytes.insert(bytes.end(), epoch.ram.begin() + run.offset, epoch.ram.begin() + run.offset + run.length);
  }
  return hash_bytes(bytes);
}

std::string to_hex(const Digest& digest) {
  std::string out;
  char buffer[4];
  for (const std::uint8_t byte : digest) {
    std::snprintf(buffer, sizeof buffer, "%02x", static_cast<unsigned>(byte));
    out += buffer;
  }
  return out;
}

AddOutcome Registry::add(const Epoch& epoch, std::uint32_t& bound_ordinal) {
  if (extents(epoch.written).empty()) {  // a plain restart: the Z80 runs the code already in RAM, i.e. the previously bound image
    bound_ordinal = last_bound_;
    if (last_bound_ != 0) return AddOutcome::restart;
    // Before any epoch an empty window means the zero-filled power-on RAM: it is itself an image (signature of no extents).
  }
  const Digest signature = signature_digest(epoch);
  for (const Image& image : images_) {
    if (image.signature != signature) continue;
    // The same signature is the same image by definition, even when the snapshot differs in carry-over data. If the differing
    // bytes were executed code, the RAM-backed guard stops the Z80 with z80_code_mismatch (ADR 0073).
    bound_ordinal = last_bound_ = image.ordinal;
    return AddOutcome::existing;
  }
  if (images_.size() >= kMaxImages) return AddOutcome::bound_exceeded;
  Image image;
  image.ordinal = static_cast<std::uint32_t>(images_.size() + 1);
  image.content = content_digest(epoch.ram);
  image.signature = signature;
  image.ram = epoch.ram;
  images_.push_back(image);
  bound_ordinal = last_bound_ = image.ordinal;
  return AddOutcome::added;
}

ExecutableImageSet executable_images(const Registry& registry, const ImageProducer& producer) {
  ExecutableImageSet set;
  set.cpu = CpuVariant::z80;
  set.platform_selector = true;  // every image maps $0000; the activation signature selects the live one (ADR 0073)
  for (const Image& image : registry.images()) {
    ExecutableImage out;
    out.id = ImageId{image.ordinal};
    out.bytes.assign(image.ram.begin(), image.ram.end());
    out.bytes.insert(out.bytes.end(), image.ram.begin(), image.ram.end());  // the 8 KiB RAM is mirrored at $2000
    out.mappings.push_back({0x0000, 0, static_cast<std::uint32_t>(kWindowBytes)});
    out.provenance.authority = producer.authority;
    out.provenance.producer = producer.name;
    out.provenance.evidence.push_back({"derivation", "runnable_epoch_snapshot"});
    out.provenance.evidence.push_back({"ordinal", std::to_string(image.ordinal)});
    out.provenance.evidence.push_back({"content_digest", to_hex(image.content)});
    out.provenance.evidence.push_back({"signature_digest", to_hex(image.signature)});
    out.provenance.evidence.push_back({"ram_mirrors", std::to_string(kWindowBytes / kRamBytes)});
    out.verification = ImageVerification::structural;
    set.images.push_back(std::move(out));
  }
  return set;
}

bool registries_equal(const Registry& left, const Registry& right) {
  const auto& a = left.images();
  const auto& b = right.images();
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (a[i].ordinal != b[i].ordinal || a[i].content != b[i].content || a[i].signature != b[i].signature || a[i].ram != b[i].ram)
      return false;
  return true;
}

codegen::z80::ImageSet build_image_set(const Registry& registry) {
  const ExecutableImageSet images = executable_images(registry);
  const std::vector<codegen::z80::ImageKind> kinds(images.images.size(), codegen::z80::ImageKind::banked);
  codegen::z80::ImageProjection projected = codegen::z80::project_executable_images(images, kinds);
  // A registry always projects (dense ordinals <= kMaxImages, one 16 KiB window at $0000); fail closed to no images otherwise.
  return projected.ok() ? std::move(projected.set) : codegen::z80::ImageSet{};
}

EmitOutcome emit_registry(const Registry& registry, const EmitRequest& request) {
  EmitOutcome outcome;
  std::error_code ec;
  std::filesystem::create_directories(request.directory, ec);
  std::vector<std::string> units;
  if (!registry.images().empty()) {
    codegen::z80::EmitOptions options = request.codegen;
    options.directory = request.directory;
    options.stem = request.stem;
    const ExecutableImageSet images = executable_images(registry);
    const std::vector<codegen::z80::ImageKind> kinds(images.images.size(), codegen::z80::ImageKind::banked);
    const codegen::z80::ImageProjection projected = codegen::z80::project_executable_images(images, kinds);
    if (!projected.ok()) {
      outcome.error = "z80 image projection: " + projected.error;
      return outcome;
    }
    const codegen::z80::EmitResult emitted = codegen::z80::emit_image_set(projected.set, options);
    if (!emitted.error.empty()) {
      outcome.error = "z80 emit: " + emitted.error;
      return outcome;
    }
    outcome.stats = emitted.stats;
    std::ifstream list(request.directory / (request.stem + ".units"));
    for (std::string line; std::getline(list, line);)
      if (!line.empty()) units.push_back(line);
  }
  const auto n = registry.images().size();
  std::ostringstream src;
  src << "/* Genesis Z80 image registry (generated; SEG-032-T003, ADR 0073). Ordinal k is the code-image identity of image k. */\n"
      << "#include <stdint.h>\n#include <string.h>\n\n"
      << "#include \"z80_registry.h\"\n\n"
      << "const uint32_t genesis_z80_image_count = " << n << "u;\n\n";
  if (n == 0) {
    src << "/* No Z80 image was materialized: the Z80 can never become runnable (z80_unknown_image), and the program carries no\n"
        << "   Z80 code at all. */\n"
        << "#include \"segarecomp/codegen/c11/runtime/z80_runtime.h\"\n"
        << "Z80Outcome z80_run(Z80Runtime *rt, uint64_t deadline) { (void)deadline; return rt->outcome = Z80_ERROR_UNKNOWN_IMAGE_IDENTITY; }\n"
        << "uint32_t genesis_z80_image_for_signature(const uint8_t signature[32]) { (void)signature; return 0u; }\n";
  } else {
    src << "static const uint8_t genesis_z80_image_signatures[" << n << "][32] = {\n";
    for (const Image& image : registry.images()) c_digest(src, image.signature);
    src << "};\n\nconst uint8_t genesis_z80_image_content_hashes[" << n << "][32] = {\n";
    for (const Image& image : registry.images()) c_digest(src, image.content);
    src << "};\n\nuint32_t genesis_z80_image_for_signature(const uint8_t signature[32]) {\n"
        << "  uint32_t index;\n  for (index = 0u; index < " << n << "u; ++index)\n"
        << "    if (memcmp(genesis_z80_image_signatures[index], signature, 32U) == 0) return index + 1u;\n  return 0u;\n}\n";
  }
  const std::string registry_name = request.stem + "_registry.c";
  if (!write_file(request.directory / registry_name, src.str())) {
    outcome.error = "cannot write the registry unit";
    return outcome;
  }
  units.push_back(registry_name);
  std::ostringstream list;
  for (const std::string& unit : units) list << unit << "\n";
  if (!write_file(request.directory / (request.stem + ".units"), list.str())) {
    outcome.error = "cannot write the unit list";
    return outcome;
  }
  return outcome;
}

}  // namespace segarecomp::machine::genesis::z80
