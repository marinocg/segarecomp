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
inline constexpr std::uint32_t kLoaderScanMaxOperations = 128U;   // SEG-098-T004 (ADR 0213): 64 -> 128, measured
inline constexpr std::uint32_t kLoaderScanMaxModules = 64U;       // the synthetic module list keeps its own bound (kModuleMaxCount = 32): above it the scan proposes modules but writes no list text
inline constexpr std::uint32_t kLoaderScanMaxStates = 4096U;
inline constexpr std::uint32_t kLoaderScanMaxSteps = 100000U;
inline constexpr std::uint32_t kLoaderScanMaxCallDepth = 8U;
inline constexpr std::uint32_t kLoaderScanMaxModuleEntries = 64U;   // SEG-099-T004: distinct proven module entries whose code the recursive walk follows
inline constexpr std::uint32_t kLoaderScanMaxReturnContexts = 64U;   // SEG-099-T003: caller contexts a joined state may summarise
inline constexpr std::uint32_t kLoaderScanSectorBytes = 2048U;
// SEG-099-T002 (ADR 0216): the bounded abstract RAM of the optional work-variable model: 256-byte pages of known bytes below the sub's I/O window (a page-count bound keeps a state small; exceeding it forgets, never invents).
inline constexpr std::uint32_t kLoaderScanMaxTrackedPages = 128U;
inline constexpr std::uint32_t kLoaderScanBiosAreaEnd = 0x6000U;      // sub PRG RAM below the SP is BIOS-owned: any modelled service may change it (ADR 0215)
inline constexpr std::uint32_t kLoaderScanCdcTrnSpan = 0x920U;        // the documented CDCTRN buffer advance; only the first 2,048 bytes are claimed as the sector

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
  computed_control_unresolved,  // SEG-098-T003: a computed jump/call (constant_dispatch mode) whose target the walk could not compute: code behind it is not inventoried
  loaded_bytes_unknown,         // SEG-099-T004: a transfer lands in a loaded range but the bytes at the target are not known to be the disc's (transformed in place, overwritten or forgotten): no source is claimed
  lookup_key_unknown,           // SEG-099-T002: immutable disc data (a directory record) was compared with a value the walk does not know: a search with an unknown key proposes no source
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
  std::uint32_t dispatch_resolved{};         // distinct computed-call/jump sites whose target was computed from known values (constant_dispatch)
  std::uint32_t memory_carried_reads{};      // distinct sites that load a data register from (An), (An)+ or d16(An) memory (not the stack): values the walk cannot know (the SP's work-variable state)
  std::uint32_t table_reads{};               // distinct immutable-image reads folded to constants (constant_dispatch)
  // SEG-099-T002 (work_state): the BIOS CD-service model and the abstract work-variable memory.
  std::uint32_t bios_modelled_calls{};       // distinct (site, function) calls to a service with a modelled effect (ADR 0215)
  std::uint32_t bios_unmodelled_calls{};     // distinct (site) calls whose effects are unknown: all abstract memory is forgotten
  std::uint32_t sector_transfers{};          // distinct CDCTRN sites that resolved source and destination
  std::uint32_t sector_transfers_unresolved{};  // distinct CDCTRN sites whose read position or destination was not constant
  std::uint32_t memory_args_resolved{};      // distinct ROMREADN sites whose {sector,count} block was read from abstract memory
  std::uint32_t memory_args_unresolved{};    // distinct ROMREADN sites whose block was not known
  std::uint32_t module_entries_walked{};     // distinct proven entries whose module code was walked (recursive_modules)
  bool convergence_used{};                   // the exact walk exhausted a bound and the result is the repeated walk with joins
  std::uint32_t joins{};                     // join operations that changed a head's joined state (state_convergence)
  std::uint32_t widenings{};                 // joins that discarded knowledge the arriving state had
  std::uint32_t async_volatile_ranges{};     // ranges the interrupt/user entries write: unknown to the init/main entries (ADR 0216)
  bool async_volatile_all{};                 // an interrupt/user entry wrote through an unknown pointer or an unmodelled service: no tracked memory is trusted
  std::uint32_t branches_decided{};          // distinct conditional-branch sites decided by tracked condition codes
  std::map<std::string, std::uint32_t> path_ends;  // distinct sites where a path ended, by class (undecodable_or_outside, computed_jump, return_to_caller, stop, exception)
};

struct LoaderScanOptions {
  // SEG-098-T003 (ADR 0212): constant-argument dispatch through immutable offset tables inside the image. The walk (a) folds ADD/SUB of known data-register values,
  // (b) reads a word/long at a known PC-relative address inside the SP image (the table is assumed immutable: a wrong assumption only yields a proposal that the
  // runtime byte guard refuses) and (c) follows `JSR/JMP (d8,PC|An,Xn)` whose index is known, so one call site with a constant selector reaches exactly one handler.
  bool constant_dispatch{false};
  // SEG-099-T002 (ADR 0216): a bounded abstract model of work-variable memory, condition codes and the documented BIOS sector-read services (ADR 0215). Off by default.
  bool work_state{false};
  // SEG-099-T003 (ADR 0217), a FALLBACK: when the exact walk exhausts a bound it is repeated, and after `widen_after` distinct states at one control-flow location the further states there are joined (knowledge is only ever lost) so a loop reaches a fixed point instead of
  // exhausting the state bound. `order` selects a test-only exploration order (0 = the default depth-first); the result must not depend on it.
  // SEG-099-T004 (ADR 0218): follow a proven transfer INTO a loaded module and find the loads that module performs (requires work_state).
  bool recursive_modules{false};
  bool state_convergence{false};
  std::uint32_t widen_after{32U};
  bool converge_always{false};   // test-only: skip the exact attempt (joins from the first walk)
  std::uint32_t order{0U};
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
