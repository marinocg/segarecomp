// SEG-030-T002 (ADR 0079 decision 3, report-only): the Genesis M68K analysis image over `genesis_m68k_executable_images`.
//
// Bytes come only from the executable-image artifact: `immutable_input` cartridge images and `static_proof` ADR 0049 alias images
// (a source reference into their cartridge image) at the alias's work-RAM execution base. Ownership follows the reachability
// challenger's decoder exactly, so the baseline discovered set is comparable: a byte is executable or immutable only when exactly
// one program-space mapping claim owns it and that claim is a cartridge image's claim; an instruction must lie wholly inside its
// cartridge mapping (or alias) and no other claim may own any of its bytes. Only cartridge bytes at their cartridge address are
// immutable; an alias execution address is mutable work RAM.

#include <algorithm>
#include <span>
#include <variant>

#include "segarecomp/cpu/m68k/decode.hpp"
#include "segarecomp/genesis_analysis_report/report.hpp"

namespace segarecomp {
namespace {

constexpr std::uint32_t bus_mask = UINT32_C(0x00FFFFFF);

bool claim_contains(const MappingClaim &claim, std::uint64_t address) {
  return claim.target_begin.space == TargetAddressSpace::m68k_program && address >= claim.target_begin.value &&
         address < claim.target_end.value;
}

}  // namespace

std::optional<GenesisM68kAnalysisImage> GenesisM68kAnalysisImage::create(const FrontendProgram &program) {
  auto images = genesis_m68k_executable_images(program);
  if (!images || images->claim_index.size() != images->set.images.size()) return std::nullopt;
  return GenesisM68kAnalysisImage{program, std::move(*images)};
}

std::optional<std::size_t> GenesisM68kAnalysisImage::unique_cartridge(std::uint32_t address) const {
  const auto &claims = program_->mapping_claims;
  std::optional<std::size_t> owner;
  for (std::size_t index = 0; index < claims.size(); ++index) {
    if (!claim_contains(claims[index], address)) continue;
    if (owner) return std::nullopt;  // more than one owner
    owner = index;
  }
  if (!owner) return std::nullopt;
  for (std::size_t image = 0; image < images_.set.images.size(); ++image)
    if (images_.set.images[image].provenance.authority == ImageAuthority::immutable_input && images_.claim_index[image] == *owner)
      return image;
  return std::nullopt;  // the unique owner is not a valid cartridge claim
}

std::optional<std::size_t> GenesisM68kAnalysisImage::alias_at(std::uint32_t pc) const {
  for (std::size_t image = 0; image < images_.set.images.size(); ++image) {
    const auto &candidate = images_.set.images[image];
    if (candidate.provenance.authority != ImageAuthority::static_proof || !candidate.source || candidate.mappings.empty()) continue;
    const auto &mapping = candidate.mappings.front();
    if (pc >= mapping.execution_base && static_cast<std::uint64_t>(pc) < static_cast<std::uint64_t>(mapping.execution_base) + mapping.length)
      return image;
  }
  return std::nullopt;
}

GenesisM68kAnalysisImage::Status GenesisM68kAnalysisImage::decode_into(std::uint32_t pc, std::optional<Instruction> &out) const {
  out.reset();
  if ((pc & 1U) != 0U) return Status::odd;
  // The instruction's first byte lives at bus address `source` (equal to `pc` except for an alias) inside cartridge image `owner`;
  // the whole instruction must stay within `span_limit` bytes.
  std::uint32_t source = pc;
  std::uint64_t span_limit = 0U;
  std::optional<std::size_t> owner;
  if (const auto alias = alias_at(pc)) {
    const auto &image = images_.set.images[*alias];
    const auto &mapping = image.mappings.front();
    const auto offset = pc - mapping.execution_base;
    const auto &source_claim = program_->mapping_claims[images_.claim_index[*alias]];
    source = source_claim.target_begin.value + image.source->offset + offset;
    owner = unique_cartridge(source);
    span_limit = static_cast<std::uint64_t>(mapping.length) - offset;
  } else {
    owner = unique_cartridge(pc);
    if (owner) span_limit = program_->mapping_claims[images_.claim_index[*owner]].target_end.value - pc;
  }
  if (!owner) return Status::unmapped;
  const auto &claim = program_->mapping_claims[images_.claim_index[*owner]];
  const auto bytes = executable_image_bytes(images_.set, images_.set.images[*owner]);
  const std::uint64_t local = static_cast<std::uint64_t>(source) - claim.target_begin.value;
  DecodeSource decode_source{CpuVariant::mc68000, {TargetAddressSpace::m68k_program, pc}, MoveqImageOffset{local}};
  auto result = decode_m68k_instruction(bytes, decode_source, M68kDecodeProfile::general_startup);
  auto *decoded = std::get_if<M68kDecodedInstruction>(&result);
  if (decoded == nullptr) return Status::rejected;
  const std::uint64_t length = decoded->provenance.length.value;
  if (length < 2U || length > span_limit || local + length > bytes.size()) return Status::rejected;
  const auto &claims = program_->mapping_claims;
  for (std::size_t index = 0; index < claims.size(); ++index) {
    if (index == images_.claim_index[*owner] || claims[index].target_begin.space != TargetAddressSpace::m68k_program) continue;
    if (source < claims[index].target_end.value && claims[index].target_begin.value < static_cast<std::uint64_t>(source) + length)
      return Status::unmapped;
  }
  decoded->provenance.source.image_offset = MoveqImageOffset{claim.image_begin.value + local};
  out = Instruction{lift_m68k_instruction(*decoded), static_cast<std::uint32_t>(length)};
  return Status::decoded;
}

std::optional<M68kAnalysisImage::Instruction> GenesisM68kAnalysisImage::decode(std::uint32_t pc) const {
  std::optional<Instruction> out;
  (void)decode_into(pc, out);
  return out;
}

GenesisM68kAnalysisImage::Status GenesisM68kAnalysisImage::status(std::uint32_t pc) const {
  std::optional<Instruction> out;
  return decode_into(pc, out);
}

bool GenesisM68kAnalysisImage::mapped(std::uint32_t pc) const {
  if ((pc & 1U) != 0U) return false;
  if (const auto alias = alias_at(pc)) {
    const auto &image = images_.set.images[*alias];
    const auto &source_claim = program_->mapping_claims[images_.claim_index[*alias]];
    return unique_cartridge(source_claim.target_begin.value + image.source->offset + (pc - image.mappings.front().execution_base))
        .has_value();
  }
  return unique_cartridge(pc).has_value();
}

std::optional<M68kRegionExtent> GenesisM68kAnalysisImage::region_of(std::uint32_t address) const {
  address &= bus_mask;
  constexpr std::uint32_t work_ram_base = UINT32_C(0xE00000), io_base = UINT32_C(0xA00000);
  if (address >= work_ram_base)
    return M68kRegionExtent{M68kRegionKind::work_ram, 0U, work_ram_base, UINT32_C(0x1000000) - work_ram_base, UINT32_C(0x10000)};
  if (address >= io_base) return M68kRegionExtent{M68kRegionKind::io_device, 0U, io_base, work_ram_base - io_base};
  const auto owner = unique_cartridge(address);
  if (!owner) return std::nullopt;
  const auto &claim = program_->mapping_claims[images_.claim_index[*owner]];
  const auto bytes = executable_image_bytes(images_.set, images_.set.images[*owner]).size();
  const std::uint64_t end = std::min<std::uint64_t>(claim.target_end.value, claim.target_begin.value + bytes);
  if (address >= end) return std::nullopt;
  return M68kRegionExtent{M68kRegionKind::image, static_cast<std::uint32_t>(*owner), static_cast<std::uint32_t>(claim.target_begin.value),
                          static_cast<std::uint32_t>(end - claim.target_begin.value)};
}

std::optional<std::uint32_t> GenesisM68kAnalysisImage::immutable_read(std::uint32_t address, unsigned bytes) const {
  // The `M68kAnalysisImage` contract admits 1, 2 or 4 bytes (the only widths the CPU owners request), exactly as the flat view.
  // The challenger's own reader also accepts 0 (an empty precise read) and 3; the owners never request either, so the D
  // equality is unaffected and the view rejects both rather than invent a value.
  if (bytes != 1U && bytes != 2U && bytes != 4U) return std::nullopt;
  std::uint32_t value = 0U;
  for (unsigned i = 0; i < bytes; ++i) {
    const auto at = (address + i) & bus_mask;
    if (at < address) return std::nullopt;  // wrapped past the bus
    // Only cartridge bytes at their cartridge address: an alias execution address is mutable work RAM.
    const auto owner = unique_cartridge(at);
    if (!owner) return std::nullopt;
    const auto &claim = program_->mapping_claims[images_.claim_index[*owner]];
    const auto image_bytes = executable_image_bytes(images_.set, images_.set.images[*owner]);
    const std::uint64_t offset = static_cast<std::uint64_t>(at) - claim.target_begin.value;
    if (offset >= image_bytes.size()) return std::nullopt;
    value = (value << 8U) | image_bytes[static_cast<std::size_t>(offset)];
  }
  return value;
}

}  // namespace segarecomp
