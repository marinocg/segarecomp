#include "segarecomp/machine/sega_cd/file_candidates.hpp"

#include <algorithm>
#include <array>

#include "segarecomp/cpu/m68k/control_successors.hpp"
#include "segarecomp/cpu/m68k/decode.hpp"
#include "segarecomp/cpu/m68k/ir.hpp"

namespace segarecomp::machine::sega_cd {

const char *file_candidate_error_name(FileCandidateError error) noexcept {
  switch (error) {
    case FileCandidateError::none: return "none";
    case FileCandidateError::no_data_track: return "no_data_track";
    case FileCandidateError::iso9660: return "iso9660";
    case FileCandidateError::too_many_entries: return "too_many_entries";
  }
  return "?";
}

FileCandidateList enumerate_file_candidates(const DiscImage &disc) {
  FileCandidateList out;
  if (disc.tracks().empty() || disc.tracks().front().type == DiscTrackType::audio) {
    out.error = FileCandidateError::no_data_track;
    return out;
  }
  const auto read = iso9660_track_reader(disc, 0);
  const auto volume = read_iso9660_volume(read, disc.tracks().front().sector_count);
  if (!volume.volume) {
    out.error = FileCandidateError::iso9660;
    out.iso_error = volume.error;
    return out;
  }
  struct Pending {
    std::string path;
    Iso9660Extent extent;
    unsigned depth;
  };
  std::vector<Pending> work{{"", volume.volume->root, 0U}};
  std::size_t next = 0;
  std::uint32_t visited = 0;
  while (next < work.size()) {
    const Pending dir = work[next++];
    ++out.directories;
    const auto listing = list_iso9660_directory(read, *volume.volume, dir.extent);
    if (listing.error != Iso9660Error::none) {
      out.error = FileCandidateError::iso9660;
      out.iso_error = listing.error;
      return out;
    }
    for (const auto &entry : listing.entries) {
      if (++visited > kFileCandidateMaxEntries) {
        out.error = FileCandidateError::too_many_entries;
        return out;
      }
      const std::string path = dir.path + "/" + entry.identifier;
      if (entry.directory) {
        if (dir.depth + 1U >= kIso9660MaxDepth) {
          out.error = FileCandidateError::iso9660;
          out.iso_error = Iso9660Error::path_invalid;
          return out;
        }
        work.push_back({path, entry.extent, dir.depth + 1U});
      } else {
        out.files.push_back({path, entry.extent});
      }
    }
  }
  return out;
}

std::vector<std::uint8_t> read_file_candidate(const DiscImage &disc, const Iso9660Extent &extent) {
  std::vector<std::uint8_t> bytes;
  const auto read = iso9660_track_reader(disc, 0);
  const std::uint32_t blocks = (extent.size + kIso9660BlockBytes - 1U) / kIso9660BlockBytes;
  bytes.reserve(static_cast<std::size_t>(blocks) * kIso9660BlockBytes);
  std::array<std::uint8_t, kIso9660BlockBytes> block{};
  for (std::uint32_t b = 0; b < blocks; ++b) {
    if (!read(extent.block + b, std::span<std::uint8_t, kIso9660BlockBytes>(block))) return {};
    bytes.insert(bytes.end(), block.begin(), block.end());
  }
  bytes.resize(extent.size);
  return bytes;
}

BroadCensus broad_census(std::span<const std::uint8_t> bytes, const BroadBodyObserver &on_body, std::span<const std::uint8_t> only) {
  BroadCensus c;
  const auto visible = static_cast<std::uint32_t>(bytes.size());
  for (std::uint32_t offset = 0; offset + 2U <= visible; offset += 2U) {
    if (!only.empty() && only[offset / 2U] == 0U) continue;
    ++c.even_offsets;
    const auto body = derive_dynamic_body(bytes, visible, offset);
    switch (body.status) {
      case DynamicBodyStatus::ok:
        ++c.compiled;
        if (on_body) on_body(offset, body);
        if (body.relative_literals != 0U) ++c.pc_dependent;
        c.generated_text_bytes += body.body.size() + body.slots.size() + body.timing.size() + body.refresh_skip.size();
        break;
      case DynamicBodyStatus::not_decoded: ++c.not_decoded; break;
      case DynamicBodyStatus::not_lifted: ++c.not_lifted; break;
      case DynamicBodyStatus::not_lowered: ++c.not_lowered; break;
      case DynamicBodyStatus::redecode: ++c.redecode; break;
      case DynamicBodyStatus::source_relative_refused:
        switch (body.derive) {
          case SourceRelativeStatus::token_structure: ++c.refused_token_structure; break;
          case SourceRelativeStatus::literal_not_linear: ++c.refused_literal_not_linear; break;
          case SourceRelativeStatus::literal_suffix: ++c.refused_literal_suffix; break;
          case SourceRelativeStatus::constant_context: ++c.refused_constant_context; break;
          case SourceRelativeStatus::ok: break;
        }
        break;
    }
  }
  return c;
}

StructuralClosure structural_closure(std::span<const std::uint8_t> bytes, StructuralOptions options) {
  constexpr std::uint32_t kP0 = 0x00200000U, kP1 = 0x00300000U;
  constexpr std::uint32_t kNone = 0xFFFFFFFFU;
  StructuralClosure out;
  const std::uint32_t n = static_cast<std::uint32_t>(bytes.size() / 2U);
  out.even_offsets = n;
  out.valid.assign(n, 0U);
  out.decoded.assign(n, 0U);
  out.defect.assign(n, 0U);
  // Per node: ok (decoded and its dependencies are in range) and up to two in-range dependencies.
  std::vector<std::array<std::uint32_t, 2>> deps(n, {kNone, kNone});
  std::vector<std::uint8_t> bad(n, 0U);
  const auto window = bytes.subspan(0, static_cast<std::size_t>(n) * 2U);
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::uint32_t offset = i * 2U;
    const auto decode_at = [&](std::uint32_t base) {
      const DecodeSource source{CpuVariant::mc68000, M68kProgramAddress{TargetAddressSpace::m68k_program, base + offset}, MoveqImageOffset{offset}};
      return decode_m68k_instruction(window, source, M68kDecodeProfile::general_startup);
    };
    const auto r0 = decode_at(kP0);
    const auto *d0 = std::get_if<M68kDecodedInstruction>(&r0);
    if (d0 == nullptr) {
      bad[i] = 1U;
      out.defect[i] = StructuralClosure::not_decoded;
      continue;
    }
    const auto r1 = decode_at(kP1);
    const auto *d1 = std::get_if<M68kDecodedInstruction>(&r1);
    const auto op0 = lift_m68k_instruction(*d0);
    if (d1 == nullptr || (d0->kind != M68kInstructionKind::moveq && op0.kind == M68kIrKind::write_moveq)) {
      bad[i] = 1U;
      out.defect[i] = StructuralClosure::not_decoded;
      continue;
    }
    ++out.decodable;
    out.decoded[i] = 1U;
    const auto op1 = lift_m68k_instruction(*d1);
    const auto s0 = m68k_control_successors(op0);
    const auto s1 = m68k_control_successors(op1);
    if (s0.always_raises_exception) {
      if (options.faults_are_terminators) {
        ++out.decodable;
        out.decoded[i] = 1U;
        continue;   // a valid instruction with no fixed successor
      }
      bad[i] = 1U;
      out.defect[i] = StructuralClosure::always_faults;
      continue;
    }
    if (s0.dynamic == M68kDynamicControlFamily::unclassified || s0.successors.size() != s1.successors.size()) {
      bad[i] = 1U;
      out.defect[i] = StructuralClosure::not_understood;
      continue;
    }
    std::uint32_t slot = 0;
    for (std::size_t k = 0; k < s0.successors.size() && bad[i] == 0U; ++k) {
      const auto t0 = s0.successors[k].target, t1 = s1.successors[k].target;
      if (t0 == t1) continue;  // absolute: a boundary
      if (t1 - t0 != kP1 - kP0) {  // neither fixed nor PC-linear: not understood
        bad[i] = 1U;
        out.defect[i] = StructuralClosure::not_understood;
        break;
      }
      const std::int64_t signed_target = static_cast<std::int64_t>(t0) - static_cast<std::int64_t>(kP0);   // PC-relative: an offset from the file's first byte
      if (signed_target < 0 || signed_target / 2 >= static_cast<std::int64_t>(n)) {
        const auto kind = s0.successors[k].kind;
        const bool sequential = kind == M68kControlSuccessorKind::fallthrough || kind == M68kControlSuccessorKind::conditional_fallthrough;
        if (options.leave_file_is_boundary && !sequential) continue;   // a call or branch into resident code at a fixed offset from the module's own base (never a fall-through off the file)
        bad[i] = 1U;
        out.defect[i] = sequential && signed_target >= 0 ? StructuralClosure::fall_off_file : StructuralClosure::target_outside_file;
        break;
      }
      const std::uint32_t target_offset = static_cast<std::uint32_t>(signed_target);
      if ((target_offset & 1U) != 0U) {
        bad[i] = 1U;  // an odd destination: not an instruction start
        out.defect[i] = StructuralClosure::target_odd;
        break;
      }
      if (slot < 2U) deps[i][slot++] = target_offset / 2U;
    }
    // A fall-through off the end of the range is an invalid flow; the fall-through is one of the successors above, so it is handled
    // by the target test (its offset is n).
  }
  // Greatest fixed point: invalidity propagates to every dependent. Reverse edges by counting sort.
  std::vector<std::uint32_t> head(static_cast<std::size_t>(n) + 1U, 0U);
  for (std::uint32_t i = 0; i < n; ++i)
    for (const auto d : deps[i])
      if (d != kNone) ++head[d + 1U];
  for (std::uint32_t i = 0; i < n; ++i) head[i + 1U] += head[i];
  std::vector<std::uint32_t> users(head[n]), fill(head.begin(), head.end() - 1);
  for (std::uint32_t i = 0; i < n; ++i)
    for (const auto d : deps[i])
      if (d != kNone) users[fill[d]++] = i;
  std::vector<std::uint32_t> work;
  for (std::uint32_t i = 0; i < n; ++i)
    if (bad[i] != 0U) work.push_back(i);
  while (!work.empty()) {
    const std::uint32_t i = work.back();
    work.pop_back();
    for (std::uint32_t k = head[i]; k < head[i + 1U]; ++k) {
      const std::uint32_t u = users[k];
      if (bad[u] == 0U) {
        bad[u] = 1U;
        out.defect[u] = out.defect[i];   // the root defect it depends on
        work.push_back(u);
      }
    }
  }
  for (std::uint32_t i = 0; i < n; ++i) {
    out.valid[i] = bad[i] == 0U ? 1U : 0U;
    out.valid_offsets += out.valid[i];
  }
  return out;
}

}  // namespace segarecomp::machine::sega_cd
