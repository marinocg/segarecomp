#include "segarecomp/rom.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace segarecomp {
namespace {
constexpr std::size_t genesis_system_offset = 0x100;
constexpr std::size_t genesis_system_size = 16;
constexpr std::size_t genesis_title_offset = 0x120;
constexpr std::size_t genesis_title_size = 48;
constexpr std::array<std::size_t, 3> eight_bit_offsets{0x1ff0, 0x3ff0, 0x7ff0};
constexpr std::string_view sms_signature = "TMR SEGA";

[[nodiscard]] bool has_range(std::span<const std::uint8_t> bytes, std::size_t start,
                             std::size_t length) {
  return start <= bytes.size() && length <= bytes.size() - start;
}
[[nodiscard]] bool matches(std::span<const std::uint8_t> bytes, std::size_t start,
                           std::string_view text) {
  return has_range(bytes, start, text.size()) &&
         std::equal(text.begin(), text.end(), bytes.begin() + static_cast<std::ptrdiff_t>(start));
}
[[nodiscard]] std::vector<std::uint8_t> copy_range(std::span<const std::uint8_t> bytes,
                                                    std::size_t start, std::size_t length) {
  if (!has_range(bytes, start, length)) return {};
  return {bytes.begin() + static_cast<std::ptrdiff_t>(start),
          bytes.begin() + static_cast<std::ptrdiff_t>(start + length)};
}
[[nodiscard]] std::string title_display(std::span<const std::uint8_t> value) {
  std::size_t end = value.size();
  while (end != 0 && value[end - 1] == 0x20) --end;
  std::ostringstream out;
  for (std::size_t index = 0; index < end; ++index) {
    const unsigned byte = value[index];
    if (byte >= 0x20U && byte <= 0x7eU) out << static_cast<char>(byte);
    else out << "\\x" << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << byte
             << std::nouppercase << std::dec;
  }
  return out.str();
}
[[nodiscard]] bool starts_sega(const std::vector<std::uint8_t> &value) {
  return value.size() >= 4 && std::equal(value.begin(), value.begin() + 4, "SEGA");
}
[[nodiscard]] HeaderCandidate genesis_candidate(std::string status) {
  HeaderCandidate candidate{};
  candidate.kind = "genesis";
  candidate.offset = genesis_system_offset;
  candidate.status = std::move(status);
  candidate.system_range = {genesis_system_offset, genesis_system_offset + genesis_system_size};
  if (candidate.status == "title_truncated" || candidate.status == "valid") {
    candidate.domestic_title_range = {genesis_title_offset, genesis_title_offset + genesis_title_size};
  }
  return candidate;
}
[[nodiscard]] HeaderCandidate eight_bit_candidate(std::size_t offset, std::string status) {
  HeaderCandidate candidate{};
  candidate.kind = "sms_gg";
  candidate.offset = offset;
  candidate.status = std::move(status);
  candidate.header_range = {offset, offset + 16};
  return candidate;
}
[[nodiscard]] Platform family_platform(std::string_view family) {
  if (family == "genesis") return Platform::genesis;
  if (family == "master_system") return Platform::master_system;
  if (family == "game_gear") return Platform::game_gear;
  return Platform::unknown;
}
[[nodiscard]] std::string hex_bytes(std::span<const std::uint8_t> bytes) {
  std::ostringstream out;
  for (const auto value : bytes) out << std::uppercase << std::hex << std::setw(2)
                                     << std::setfill('0') << static_cast<unsigned>(value);
  return out.str();
}
[[nodiscard]] const char *range_status_name(ResetRangeStatus status) noexcept {
  switch (status) { case ResetRangeStatus::unexamined: return "unexamined"; case ResetRangeStatus::unavailable: return "unavailable"; case ResetRangeStatus::available: return "available"; }
  return "unexamined";
}
[[nodiscard]] const char *check_status_name(ResetCheckStatus status) noexcept {
  switch (status) { case ResetCheckStatus::unexamined: return "unexamined"; case ResetCheckStatus::not_checked: return "not_checked"; case ResetCheckStatus::even: return "even"; case ResetCheckStatus::odd: return "odd"; case ResetCheckStatus::mapped: return "mapped"; case ResetCheckStatus::unmapped: return "unmapped"; }
  return "unexamined";
}
[[nodiscard]] std::uint32_t be32(const std::array<std::uint8_t, 4> &bytes) noexcept {
  return (static_cast<std::uint32_t>(bytes[0]) << 24U) | (static_cast<std::uint32_t>(bytes[1]) << 16U) |
         (static_cast<std::uint32_t>(bytes[2]) << 8U) | static_cast<std::uint32_t>(bytes[3]);
}
void write_optional_number(std::ostringstream &out, const std::optional<VectorWord32> &value) { if (value) out << value->value; else out << "null"; }
void write_optional_address(std::ostringstream &out, const std::optional<M68kAddress24> &value) { if (value) out << value->value; else out << "null"; }
void write_optional_offset(std::ostringstream &out, const std::optional<ImageOffset> &value) { if (value) out << value->value; else out << "null"; }
void write_range(std::ostringstream &out, const ResetRange &range) { out << "{\"start\":" << range.start << ",\"end\":" << range.end << ",\"status\":\"" << range_status_name(range.status) << "\"}"; }
void write_vector(std::ostringstream &out, const ResetVectorProvenance &vector) {
  out << "{\"offset\":" << vector.offset.value << ",\"length\":" << vector.length << ",\"status\":\"" << range_status_name(vector.status) << "\",\"bytes\":";
  if (vector.bytes) out << "[" << static_cast<unsigned>((*vector.bytes)[0]) << "," << static_cast<unsigned>((*vector.bytes)[1]) << "," << static_cast<unsigned>((*vector.bytes)[2]) << "," << static_cast<unsigned>((*vector.bytes)[3]) << "]"; else out << "null";
  out << ",\"word\":"; write_optional_number(out, vector.word); out << "}";
}
} // namespace

std::vector<std::uint8_t> read_binary(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open input: " + path.string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

RomInfo inspect_rom(std::span<const std::uint8_t> bytes) {
  RomInfo result{};
  result.size = bytes.size();
  if (bytes.size() > image_size_limit) {
    result.outcome = ClassificationOutcome::rejected;
    result.diagnostic = Diagnostic::size_limit;
    return result;
  }

  constexpr std::string_view mega_drive = "SEGA MEGA DRIVE ";
  constexpr std::string_view genesis = "SEGA GENESIS    ";
  if (has_range(bytes, genesis_system_offset, genesis_system_size)) {
    const auto system = copy_range(bytes, genesis_system_offset, genesis_system_size);
    if (std::equal(system.begin(), system.end(), mega_drive.begin()) ||
        std::equal(system.begin(), system.end(), genesis.begin())) {
      if (!has_range(bytes, genesis_title_offset, genesis_title_size)) {
        auto candidate = genesis_candidate("title_truncated");
        candidate.system_bytes = system;
        result.candidates.push_back(std::move(candidate));
      } else {
        auto candidate = genesis_candidate("valid");
        candidate.system_bytes = system;
        candidate.domestic_title_bytes = copy_range(bytes, genesis_title_offset, genesis_title_size);
        candidate.family = "genesis";
        candidate.system_type = system == std::vector<std::uint8_t>(mega_drive.begin(), mega_drive.end())
                                    ? "mega_drive" : "genesis";
        result.candidates.push_back(std::move(candidate));
      }
    } else if (starts_sega(system)) {
      auto candidate = genesis_candidate("unsupported_system");
      candidate.system_bytes = system;
      result.candidates.push_back(std::move(candidate));
    }
  } else if (bytes.size() >= genesis_system_offset + 4 && bytes.size() < genesis_system_offset + genesis_system_size) {
    const auto partial = copy_range(bytes, genesis_system_offset, bytes.size() - genesis_system_offset);
    const bool mega_prefix = std::equal(partial.begin(), partial.end(), mega_drive.begin());
    const bool genesis_prefix = std::equal(partial.begin(), partial.end(), genesis.begin());
    if (mega_prefix || genesis_prefix) {
      auto candidate = genesis_candidate("system_truncated");
      candidate.system_bytes = partial;
      result.candidates.push_back(std::move(candidate));
    }
  }

  for (const auto offset : eight_bit_offsets) {
    if (!has_range(bytes, offset, sms_signature.size()) || !matches(bytes, offset, sms_signature)) continue;
    if (!has_range(bytes, offset, 16)) {
      result.candidates.push_back(eight_bit_candidate(offset, "truncated"));
      continue;
    }
    const unsigned region = static_cast<unsigned>(bytes[offset + 15] >> 4U);
    auto candidate = eight_bit_candidate(offset, "valid");
    candidate.rom_size_code = static_cast<unsigned>(bytes[offset + 15] & 0x0fU);
    switch (region) {
    case 3: candidate.family = "master_system"; candidate.region_system = "sms_japan"; break;
    case 4: candidate.family = "master_system"; candidate.region_system = "sms_export"; break;
    case 5: candidate.family = "game_gear"; candidate.region_system = "gg_japan"; break;
    case 6: candidate.family = "game_gear"; candidate.region_system = "gg_export"; break;
    case 7: candidate.family = "game_gear"; candidate.region_system = "gg_international"; break;
    default: candidate.status = "invalid_region"; break;
    }
    result.candidates.push_back(std::move(candidate));
  }

  const auto first_status = [&result](std::string_view status) -> const HeaderCandidate * {
    const auto found = std::find_if(result.candidates.begin(), result.candidates.end(),
                                    [status](const HeaderCandidate &c) { return c.status == status; });
    return found == result.candidates.end() ? nullptr : &*found;
  };
  if (const auto *candidate = first_status("system_truncated")) {
    (void)candidate; result.outcome = ClassificationOutcome::rejected; result.diagnostic = Diagnostic::genesis_system_truncated;
  } else if (const auto *candidate = first_status("title_truncated")) {
    (void)candidate; result.outcome = ClassificationOutcome::rejected; result.diagnostic = Diagnostic::genesis_domestic_title_truncated;
  } else if (const auto *candidate = first_status("truncated")) {
    (void)candidate; result.outcome = ClassificationOutcome::rejected; result.diagnostic = Diagnostic::sms_gg_header_truncated;
  } else if (first_status("unsupported_system") != nullptr) {
    result.outcome = ClassificationOutcome::rejected; result.diagnostic = Diagnostic::genesis_system_unsupported;
  } else if (first_status("invalid_region") != nullptr) {
    result.outcome = ClassificationOutcome::rejected; result.diagnostic = Diagnostic::sms_gg_region_invalid;
  } else {
    std::vector<const HeaderCandidate *> valid;
    for (const auto &candidate : result.candidates) if (candidate.status == "valid") valid.push_back(&candidate);
    if (valid.size() == 1) {
      const auto &candidate = *valid.front();
      result.outcome = ClassificationOutcome::recognized;
      result.family = candidate.family; result.platform = family_platform(candidate.family);
      result.system_type = candidate.system_type; result.region_system = candidate.region_system;
      result.rom_size_code = candidate.rom_size_code; result.domestic_title_bytes = candidate.domestic_title_bytes;
      if (candidate.kind == "genesis") {
        result.domestic_title_display = title_display(candidate.domestic_title_bytes);
        result.domestic_title_encoding = "ascii_with_hex_escapes";
        result.diagnostic = Diagnostic::recognized_genesis;
      } else result.diagnostic = candidate.family == "master_system" ? Diagnostic::recognized_sms : Diagnostic::recognized_gg;
    } else if (valid.size() >= 2) {
      result.outcome = ClassificationOutcome::rejected;
      const auto family = valid.front()->family;
      result.diagnostic = std::all_of(valid.begin(), valid.end(), [family](const HeaderCandidate *c) { return c->family == family; })
                              ? Diagnostic::header_collision : Diagnostic::header_conflict;
    }
  }
  return result;
}

const char *platform_name(Platform platform) noexcept { switch (platform) {
case Platform::genesis: return "Genesis / Mega Drive"; case Platform::master_system: return "Master System";
case Platform::game_gear: return "Game Gear"; case Platform::unknown: return "Unknown"; } return "Unknown"; }
const char *outcome_name(ClassificationOutcome outcome) noexcept { switch (outcome) {
case ClassificationOutcome::recognized: return "recognized"; case ClassificationOutcome::rejected: return "rejected";
case ClassificationOutcome::unrecognized: return "unrecognized"; } return "unrecognized"; }
const char *diagnostic_name(Diagnostic diagnostic) noexcept { switch (diagnostic) {
case Diagnostic::recognized_genesis: return "HDR_RECOGNIZED_GENESIS"; case Diagnostic::recognized_sms: return "HDR_RECOGNIZED_SMS";
case Diagnostic::recognized_gg: return "HDR_RECOGNIZED_GG"; case Diagnostic::size_limit: return "IMG_SIZE_LIMIT";
case Diagnostic::genesis_system_truncated: return "HDR_GENESIS_SYSTEM_TRUNCATED"; case Diagnostic::genesis_domestic_title_truncated: return "HDR_GENESIS_DOMESTIC_TITLE_TRUNCATED";
case Diagnostic::genesis_system_unsupported: return "HDR_GENESIS_SYSTEM_UNSUPPORTED"; case Diagnostic::sms_gg_header_truncated: return "HDR_8BIT_HEADER_TRUNCATED";
case Diagnostic::sms_gg_region_invalid: return "HDR_SMS_GG_REGION_INVALID"; case Diagnostic::header_collision: return "HDR_HEADER_COLLISION";
case Diagnostic::header_conflict: return "HDR_HEADER_CONFLICT"; case Diagnostic::not_recognized: return "HDR_NOT_RECOGNIZED"; } return "HDR_NOT_RECOGNIZED"; }

std::string format_inspection(const RomInfo &rom) {
  std::ostringstream out;
  out << "input-size: " << rom.size << "\nsize-limit: " << image_size_limit << "\noutcome: " << outcome_name(rom.outcome)
      << "\ndiagnostic: " << diagnostic_name(rom.diagnostic) << "\ncandidates:\n";
  for (const auto &c : rom.candidates) {
    out << "- kind: " << c.kind << "\n  offset: 0x" << std::uppercase << std::hex << c.offset << std::dec
        << "\n  status: " << c.status << "\n";
    if (c.kind == "genesis") {
      out << "  system-range: [0x100,0x110)\n";
      if (c.domestic_title_range.end != 0) out << "  domestic-title-range: [0x120,0x150)\n";
      out << "  system-bytes: " << hex_bytes(c.system_bytes) << "\n  domestic-title-bytes: " << hex_bytes(c.domestic_title_bytes) << "\n";
    }
    else out << "  header-range: [0x" << std::uppercase << std::hex << c.header_range.start << ",0x" << c.header_range.end << std::dec << ")\n";
  }
  if (rom.outcome == ClassificationOutcome::recognized) {
    out << "family: " << rom.family << "\n";
    if (rom.family == "genesis") out << "system-type: " << rom.system_type << "\ndomestic-title-bytes: " << hex_bytes(rom.domestic_title_bytes)
                                       << "\ndomestic-title-display: " << rom.domestic_title_display << "\ndomestic-title-encoding: " << rom.domestic_title_encoding << "\n";
    else out << "region-system: " << rom.region_system << "\nrom-size-code: " << rom.rom_size_code << "\n";
  }
  return out.str();
}

const char *reset_diagnostic_name(ResetDiagnostic diagnostic) noexcept { switch (diagnostic) {
case ResetDiagnostic::image_size_limit: return "GENESIS_IMAGE_SIZE_LIMIT";
case ResetDiagnostic::reset_ssp_truncated: return "GENESIS_RESET_SSP_TRUNCATED";
case ResetDiagnostic::reset_pc_truncated: return "GENESIS_RESET_PC_TRUNCATED";
case ResetDiagnostic::reset_pc_not_24bit: return "GENESIS_RESET_PC_NOT_24BIT";
case ResetDiagnostic::reset_pc_odd: return "GENESIS_RESET_PC_ODD";
case ResetDiagnostic::reset_pc_unmapped: return "GENESIS_RESET_PC_UNMAPPED";
case ResetDiagnostic::reset_image_accepted: return "GENESIS_RESET_IMAGE_ACCEPTED"; } return "GENESIS_RESET_SSP_TRUNCATED"; }

GenesisResetImageReport analyze_genesis_reset_image(std::span<const std::uint8_t> bytes) {
  GenesisResetImageReport result{}; result.input_size = bytes.size();
  if (bytes.size() > image_size_limit) { result.diagnostic = ResetDiagnostic::image_size_limit; return result; }
  result.raw_region_constructed = true;
  result.pc_alignment = ResetCheckStatus::not_checked;
  result.pc_mapping = ResetCheckStatus::not_checked;
  const auto read_vector = [&bytes](ImageOffset offset) -> std::optional<std::array<std::uint8_t, 4>> {
    if (!has_range(bytes, offset.value, 4)) return std::nullopt;
    return std::array<std::uint8_t, 4>{bytes[offset.value], bytes[offset.value + 1], bytes[offset.value + 2], bytes[offset.value + 3]};
  };
  const auto ssp_bytes = read_vector(ImageOffset{0});
  result.reset_ssp_range.status = ssp_bytes ? ResetRangeStatus::available : ResetRangeStatus::unavailable;
  result.ssp.status = result.reset_ssp_range.status;
  if (!ssp_bytes) { result.reset_pc_range.status = ResetRangeStatus::unavailable; result.pc.status = ResetRangeStatus::unavailable; return result; }
  result.ssp.bytes = ssp_bytes; result.ssp.word = VectorWord32{be32(*ssp_bytes)}; result.initial_ssp = result.ssp.word;
  const auto pc_bytes = read_vector(ImageOffset{4});
  result.reset_pc_range.status = pc_bytes ? ResetRangeStatus::available : ResetRangeStatus::unavailable;
  result.pc.status = result.reset_pc_range.status;
  if (!pc_bytes) { result.diagnostic = ResetDiagnostic::reset_pc_truncated; return result; }
  result.pc.bytes = pc_bytes; result.pc.word = VectorWord32{be32(*pc_bytes)}; result.initial_pc_word = result.pc.word;
  const auto pc_word = result.initial_pc_word->value;
  if ((pc_word & 0xff000000U) != 0U) { result.diagnostic = ResetDiagnostic::reset_pc_not_24bit; result.pc_alignment = ResetCheckStatus::not_checked; result.pc_mapping = ResetCheckStatus::not_checked; return result; }
  result.initial_pc = M68kAddress24{pc_word};
  if ((pc_word & 1U) != 0U) { result.diagnostic = ResetDiagnostic::reset_pc_odd; result.pc_alignment = ResetCheckStatus::odd; result.pc_mapping = ResetCheckStatus::not_checked; return result; }
  result.pc_alignment = ResetCheckStatus::even;
  if (static_cast<std::size_t>(pc_word) >= bytes.size()) { result.diagnostic = ResetDiagnostic::reset_pc_unmapped; result.pc_mapping = ResetCheckStatus::unmapped; return result; }
  result.outcome = ResetOutcome::accepted; result.diagnostic = ResetDiagnostic::reset_image_accepted; result.pc_mapping = ResetCheckStatus::mapped;
  result.entry_address = result.initial_pc; result.entry_image_offset = ImageOffset{static_cast<std::size_t>(pc_word)};
  return result;
}

std::string format_genesis_reset_image_report(const GenesisResetImageReport &report) {
  std::ostringstream out;
  out << "{\"outcome\":\"" << (report.outcome == ResetOutcome::accepted ? "accepted" : "rejected") << "\",\"diagnostic\":\"" << reset_diagnostic_name(report.diagnostic) << "\",\"exit_class\":\"" << (report.outcome == ResetOutcome::accepted ? "success" : "input_rejected") << "\",\"input_size\":" << report.input_size << ",\"size_limit\":" << image_size_limit << ",\"raw_region_status\":\"" << (report.raw_region_constructed ? "constructed" : "not_constructed") << "\",\"raw_region\":";
  if (report.raw_region_constructed) out << "{\"name\":\"raw_cartridge_rom\",\"cpu_address_range\":[0," << report.input_size << "],\"image_range\":[0," << report.input_size << "],\"permissions\":[\"read\",\"execute\"],\"mapping\":\"identity_non_mirrored\"}"; else out << "null";
  out << ",\"reset_ssp_range\":"; write_range(out, report.reset_ssp_range); out << ",\"reset_pc_range\":"; write_range(out, report.reset_pc_range);
  out << ",\"initial_ssp\":"; write_optional_number(out, report.initial_ssp); out << ",\"initial_pc_word\":"; write_optional_number(out, report.initial_pc_word); out << ",\"initial_pc\":"; write_optional_address(out, report.initial_pc); out << ",\"entry_address\":"; write_optional_address(out, report.entry_address); out << ",\"entry_image_offset\":"; write_optional_offset(out, report.entry_image_offset);
  out << ",\"pc_alignment\":\"" << check_status_name(report.pc_alignment) << "\",\"pc_mapping\":\"" << check_status_name(report.pc_mapping) << "\",\"source_provenance\":{\"input\":\"raw_image\",\"byte_order\":\"big_endian\",\"source_ids\":[\"M1\",\"M2\",\"M3\",\"S1\",\"S2\"],\"ssp\":"; write_vector(out, report.ssp); out << ",\"pc\":"; write_vector(out, report.pc); out << "}}";
  return out.str();
}
} // namespace segarecomp
