#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace segarecomp {

enum class Platform { unknown, genesis, master_system, game_gear };
enum class ClassificationOutcome { recognized, rejected, unrecognized };
enum class Diagnostic {
  recognized_genesis,
  recognized_sms,
  recognized_gg,
  size_limit,
  genesis_system_truncated,
  genesis_domestic_title_truncated,
  genesis_system_unsupported,
  sms_gg_header_truncated,
  sms_gg_region_invalid,
  header_collision,
  header_conflict,
  not_recognized,
};

struct ByteRange {
  std::size_t start{};
  std::size_t end{};
};

struct HeaderCandidate {
  std::string kind;
  std::size_t offset{};
  std::string status;
  ByteRange header_range{};
  ByteRange system_range{};
  ByteRange domestic_title_range{};
  std::vector<std::uint8_t> system_bytes;
  std::vector<std::uint8_t> domestic_title_bytes;
  std::string family;
  std::string system_type;
  std::string region_system;
  unsigned rom_size_code{};
};

struct RomInfo {
  Platform platform{Platform::unknown};
  std::size_t size{};
  ClassificationOutcome outcome{ClassificationOutcome::unrecognized};
  Diagnostic diagnostic{Diagnostic::not_recognized};
  std::vector<HeaderCandidate> candidates;
  std::string family;
  std::string system_type;
  std::vector<std::uint8_t> domestic_title_bytes;
  std::string domestic_title_display;
  std::string domestic_title_encoding;
  std::string region_system;
  unsigned rom_size_code{};
};

inline constexpr std::size_t image_size_limit = 0x400000;

// Target-domain values; these are never host pointers.
struct ImageOffset { std::size_t value{}; };
struct M68kAddress24 { std::uint32_t value{}; };
struct VectorWord32 { std::uint32_t value{}; };

enum class ResetOutcome { accepted, rejected };
enum class ResetDiagnostic {
  image_size_limit, reset_ssp_truncated, reset_pc_truncated, reset_pc_not_24bit,
  reset_pc_odd, reset_pc_unmapped, reset_image_accepted,
};
enum class ResetRangeStatus { unexamined, unavailable, available };
enum class ResetCheckStatus { unexamined, not_checked, even, odd, mapped, unmapped };

struct ResetRange {
  std::size_t start{};
  std::size_t end{};
  ResetRangeStatus status{ResetRangeStatus::unexamined};
};
struct ResetVectorProvenance {
  ImageOffset offset{};
  std::size_t length{4};
  ResetRangeStatus status{ResetRangeStatus::unexamined};
  std::optional<std::array<std::uint8_t, 4>> bytes;
  std::optional<VectorWord32> word;
};
struct GenesisResetImageReport {
  ResetOutcome outcome{ResetOutcome::rejected};
  ResetDiagnostic diagnostic{ResetDiagnostic::reset_ssp_truncated};
  std::size_t input_size{};
  bool raw_region_constructed{};
  ResetRange reset_ssp_range{0, 4, ResetRangeStatus::unexamined};
  ResetRange reset_pc_range{4, 8, ResetRangeStatus::unexamined};
  std::optional<VectorWord32> initial_ssp;
  std::optional<VectorWord32> initial_pc_word;
  std::optional<M68kAddress24> initial_pc;
  std::optional<M68kAddress24> entry_address;
  std::optional<ImageOffset> entry_image_offset;
  ResetCheckStatus pc_alignment{ResetCheckStatus::unexamined};
  ResetCheckStatus pc_mapping{ResetCheckStatus::unexamined};
  ResetVectorProvenance ssp{ImageOffset{0}, 4, ResetRangeStatus::unexamined, std::nullopt, std::nullopt};
  ResetVectorProvenance pc{ImageOffset{4}, 4, ResetRangeStatus::unexamined, std::nullopt, std::nullopt};
};

[[nodiscard]] std::vector<std::uint8_t> read_binary(const std::filesystem::path &path);
[[nodiscard]] RomInfo inspect_rom(std::span<const std::uint8_t> bytes);
[[nodiscard]] const char *platform_name(Platform platform) noexcept;
[[nodiscard]] const char *outcome_name(ClassificationOutcome outcome) noexcept;
[[nodiscard]] const char *diagnostic_name(Diagnostic diagnostic) noexcept;
[[nodiscard]] std::string format_inspection(const RomInfo &rom);
[[nodiscard]] GenesisResetImageReport analyze_genesis_reset_image(std::span<const std::uint8_t> bytes);
[[nodiscard]] const char *reset_diagnostic_name(ResetDiagnostic diagnostic) noexcept;
[[nodiscard]] std::string format_genesis_reset_image_report(const GenesisResetImageReport &report);

} // namespace segarecomp
