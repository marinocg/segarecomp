#pragma once

// SEG-097-T001 (ADR 0206): static discovery of disc-loaded executable modules that no run has loaded.
//
// A bounded, path-sensitive constant-propagation walk of the admitted sub boot image over the CPU-owned decode / lift / effect /
// control-successor APIs recognises the machine's documented CD-load operation (CDD play command, CDC destination, DBC, DMA address,
// DTRG; contract section 7) and the control transfers that later land in the loaded range. Both arms of every conditional branch are
// explored, so a module chosen by runtime input is found without being executed. The scan only PROPOSES: its output is the existing
// digest-checked module list, admitted unchanged by `admit_modules`, and every execution is still verified against live bytes.
//
// It models no memory except the CD hardware registers, no interpreter state beyond constants, and nothing is decoded at run time.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "segarecomp/disc.hpp"
#include "segarecomp/machine/sega_cd/boot_images.hpp"

namespace segarecomp::machine::sega_cd {

// Resource controls (ADR 0206 decision 7): exhaustion is a typed refusal and marks the scan incomplete, never a silent truncation.
inline constexpr std::uint32_t kLoaderScanMaxOperations = 64U;
inline constexpr std::uint32_t kLoaderScanMaxStates = 4096U;
inline constexpr std::uint32_t kLoaderScanMaxSteps = 100000U;
inline constexpr std::uint32_t kLoaderScanMaxCallDepth = 8U;
inline constexpr std::uint32_t kLoaderScanSectorBytes = 2048U;

enum class LoaderRefusalKind : std::uint8_t {
  lba_unknown,                // the CDD play position was not a constant at the transfer
  destination_unknown,        // the CDC destination or DMA address was not a constant at the transfer
  destination_unsupported,    // a destination other than Word RAM, or a range leaving the sub's 2M Word RAM window
  length_unsupported,         // a transfer that is not exactly one 2,048-byte sector
  cdd_command_invalid,        // a play command whose digits or checksum are invalid
  cdc_state_unknown,          // the CDC register pointer or data-output enable was not known at the transfer
  loaded_not_executed,        // a sector was loaded and no proven control transfer lands in it: data, not a module
  entry_unknown,              // a computed transfer after a load whose target is not a constant
  entry_not_code,             // the proven entry does not decode as an instruction of the loaded bytes
  source_outside_track,       // the sector is not a data block of track 1 outside the system area
  too_many_operations,
  too_many_modules,
  scan_limit_exceeded,        // states, steps or call depth exhausted
};
[[nodiscard]] const char *loader_refusal_name(LoaderRefusalKind kind) noexcept;

struct LoaderRefusal {
  LoaderRefusalKind kind{};
  std::uint32_t pc{};  // the boot-image address of the trigger (a build-time address of the disc's own SP image)
};

// One recognised load: track-1 sector -> sub Word RAM, with the control-transfer targets proven to land in it.
struct LoadOperation {
  std::uint32_t trigger_pc{};
  std::uint32_t lba{};          // block of track 1
  std::uint32_t destination{};  // sub-bus address of the first byte
  std::uint32_t bytes{};
  std::vector<std::uint32_t> entries;  // absolute guest addresses proven to be transfer targets inside [destination, destination + bytes)
};

struct DiscoveredModule {
  std::string name;  // derived from the source coordinate ("lba<N>[_<k>]"), never from a file name
  Processor processor{Processor::sub};
  std::uint32_t block{};
  std::uint32_t size{};
  std::string sha256;
  std::vector<std::uint32_t> bases;    // execution address of image offset 0
  std::vector<std::uint32_t> entries;  // image offsets
  std::uint32_t range_offset{};        // executable subrange
  std::uint32_t range_length{};
};

// SEG-098-T001 (ADR 0210): aggregate, non-reconstructable survey counters gathered by the same walk (classes and counts only; no
// address, name or byte of the disc is retained here beyond the BIOS service function numbers, which are the public BIOS API).
struct LoaderSurvey {
  std::uint32_t roots{};                     // walk roots (the descriptor entry, or the SP module jump-table entries)
  bool module_table_found{};                 // the SP carries a valid module jump table (header word at +$18)
  std::uint32_t cdd_commands{};              // distinct sites writing the CDD command word (TX8/TX9)
  std::uint32_t cdc_transfer_triggers{};     // distinct sites writing the CDC transfer trigger
  std::uint32_t bios_service_calls{};        // distinct (site, function) calls into the BIOS area below the SP image
  std::uint32_t bios_service_unknown_function{};
  std::map<std::uint32_t, std::uint32_t> bios_functions;  // D0.w function number -> distinct call sites
  std::uint32_t foreign_calls_other{};       // distinct constant calls/jumps to other targets outside the image
  std::uint32_t foreign_calls_unknown{};     // distinct computed calls/jumps whose target is unknown
  std::uint32_t copy_loop_candidates{};      // distinct (An)+,(Am)+ moves reached (CPU copy candidates)
  std::uint32_t counted_loops{};             // distinct DBcc sites reached
  std::map<std::string, std::uint32_t> path_ends;  // distinct sites where a path ended, by class (undecodable_or_outside, computed_jump, return_to_caller, stop, exception)
};

struct LoaderScanOptions {
  bool module_table_roots{false};  // walk the SP module jump table (init, main, int2, user) instead of only the descriptor entry
  bool continue_after_computed_call{false};  // survey what-if: a computed call (JSR (d8,An,Xn) / (d8,PC,Xn)) is a foreign call that returns, so the walk continues after it
};

struct LoaderScan {
  std::vector<LoadOperation> operations;
  std::vector<DiscoveredModule> modules;
  std::vector<LoaderRefusal> refusals;
  bool complete{true};  // false when a resource limit was reached: the inventory must not be labelled complete
  std::uint32_t states{};
  std::uint32_t steps{};
  std::string manifest;  // canonical module list text (`segarecomp.sega_cd.modules.v1`); empty when no module was discovered
  std::string identity;  // SHA-256 of the manifest text
  LoaderSurvey survey;
};

// Pure function of the disc and its admitted boot images. Executes no guest instruction and reads from the disc only the sectors it digests.
[[nodiscard]] LoaderScan scan_sega_cd_loaders(const DiscImage &disc, const BootImages &images, const LoaderScanOptions &options = {});

}  // namespace segarecomp::machine::sega_cd
