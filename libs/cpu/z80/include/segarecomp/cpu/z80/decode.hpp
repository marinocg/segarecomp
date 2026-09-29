#pragma once

// Generation-time Z80 decoder over a logical fetch function (ADR 0058 section 5). Bytes are fetched at
// successive 16-bit logical addresses (wrapping 0xFFFF -> 0x0000); a storage-image edge is never architectural.

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "segarecomp/cpu/z80/forms.hpp"

namespace segarecomp::cpu::z80 {

enum class FetchKind : std::uint8_t {
  byte,                // resolved code byte
  unresolved_mapping,  // the address lies in a different mapping-sensitive window
  non_code,            // the address is not immutable code
};

struct FetchedByte {
  FetchKind kind{FetchKind::non_code};
  std::uint8_t value{};
  std::uint32_t image_id{};      // code-image identity of the resolved byte
  std::uint32_t image_offset{};  // offset of the byte in that image
  friend bool operator==(const FetchedByte&, const FetchedByte&) = default;
};

// Caller-supplied logical code mapping. `fetch` must be a pure function of the address.
class LogicalFetch {
 public:
  virtual ~LogicalFetch() = default;
  virtual FetchedByte fetch(std::uint16_t address) const = 0;
};

// A dense 65,536-entry mapping (also what `classify_all` snapshots the caller's mapping into).
class TableFetch final : public LogicalFetch {
 public:
  TableFetch();
  FetchedByte fetch(std::uint16_t address) const override { return table_[address]; }
  void set(std::uint16_t address, const FetchedByte& byte) { table_[address] = byte; }
  // Every address a resolved byte of `image_id`, value = fill, offset = address.
  static TableFetch invariant(std::uint32_t image_id, std::uint8_t fill);

 private:
  std::vector<FetchedByte> table_;
};

enum class EffectivePrefix : std::uint8_t { none, dd, fd };

// Z80-owned provenance (ADR 0059). Bytes are not assumed contiguous in storage: replay the logical fetch
// function from `address` for `logical_byte_count` bytes to recover them.
struct Provenance {
  std::uint32_t image_id{};      // start's code-image identity
  std::uint16_t address{};       // start's 16-bit logical address
  std::uint32_t image_offset{};  // start's offset in its image
  std::uint32_t logical_byte_count{};
  std::uint32_t prefix_count{};  // DD/FD prefixes at the start of the run (any length)
  EffectivePrefix effective_prefix{EffectivePrefix::none};
  FormId form{kNoForm};
  std::uint8_t opcode{};                       // canonical opcode byte (last byte of the encoding for DDCB/FDCB)
  std::array<std::uint8_t, 3> operands{};      // displacement then immediate bytes, as fetched
  std::uint8_t operand_count{};
  friend bool operator==(const Provenance&, const Provenance&) = default;
};

struct DecodedInstruction {
  Provenance provenance;
  FormId form{kNoForm};
  // Superseded/ignored prefixes beyond the form's own canonical prefix: each costs 4 T-states and one M1 fetch.
  std::uint32_t extra_prefix_count{};
  friend bool operator==(const DecodedInstruction&, const DecodedInstruction&) = default;
};

enum class StartKind : std::uint8_t {
  decoded,
  prefix_lock,               // an endless DD/FD run
  unresolved_fetch_mapping,  // bytes continue into a different mapping-sensitive window
  mutable_code,              // bytes continue into non-code
  excluded_form,             // reserved; the T001 scope excludes nothing
};

struct StartClassification {
  StartKind kind{StartKind::excluded_form};
  DecodedInstruction instruction;      // valid iff kind == decoded
  std::uint16_t blocking_address{};    // the offending logical address for unresolved/mutable
  friend bool operator==(const StartClassification&, const StartClassification&) = default;
};

// Classify one start. Work is bounded by 65,536 fetches (a DD/FD-only run of the whole space is prefix_lock).
StartClassification decode_at(const LogicalFetch& fetch, std::uint16_t address);

// Classify all 65,536 starts in linear time: the mapping is fetched once per address and prefix-run ends are
// precomputed over the circular logical address space. Result is indexed by start address.
std::vector<StartClassification> classify_all(const LogicalFetch& fetch);

// Replay the logical bytes of a decoded start through the fetch function (nullopt if any byte no longer resolves).
std::optional<std::vector<std::uint8_t>> replay_bytes(const LogicalFetch& fetch, const Provenance& provenance);

// Total instruction length in logical bytes = extra_prefix_count + form length.
std::uint32_t logical_length(const DecodedInstruction& instruction);
std::optional<std::int8_t> displacement(const DecodedInstruction& instruction);
std::optional<std::uint16_t> immediate(const DecodedInstruction& instruction);

enum class TimingOutcome : std::uint8_t { primary, alternate };
// T-states of one execution: the form's timing (see Timing) plus 4 per superseded/ignored prefix.
std::uint64_t t_states(const DecodedInstruction& instruction, TimingOutcome outcome);
// M1 fetches (R increment) of the instruction, prefixes included.
std::uint32_t m1_fetches(const DecodedInstruction& instruction);

}  // namespace segarecomp::cpu::z80
