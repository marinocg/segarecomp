#include "segarecomp/machine/genesis/m68k_copy_alias.hpp"

#include <algorithm>
#include <functional>

namespace segarecomp::machine::genesis::m68k_alias {

std::optional<GuestStopRecord> parse_stop_record(std::span<const std::uint8_t> bytes) {
  if (bytes.size() != kStopRecordBytes) return std::nullopt;
  const auto be32 = [&](std::size_t at) {
    return (static_cast<std::uint32_t>(bytes[at]) << 24) | (static_cast<std::uint32_t>(bytes[at + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 8) | static_cast<std::uint32_t>(bytes[at + 3]);
  };
  GuestStopRecord record;
  record.pc = be32(0);
  record.stop_class = be32(4);
  record.work_ram.assign(bytes.begin() + 8, bytes.end());
  return record;
}

std::optional<CopyAlias> derive_copy_alias(std::span<const std::uint8_t> rom, std::span<const std::uint8_t> work_ram,
                                           std::uint32_t pc) {
  if ((pc & 1U) != 0 || pc < kWorkRamBegin || pc >= kWorkRamBegin + kWorkRamBytes) return std::nullopt;
  const std::size_t offset = pc - kWorkRamBegin;
  if (offset > work_ram.size() || work_ram.size() - offset < kMinRun) return std::nullopt;
  const auto needle_begin = work_ram.begin() + static_cast<std::ptrdiff_t>(offset);
  const auto needle_end = needle_begin + kMinRun;
  const std::boyer_moore_horspool_searcher searcher(needle_begin, needle_end);
  struct Best { std::size_t total, back, position; };
  std::optional<Best> best;
  for (auto from = rom.begin();;) {
    const auto found = std::search(from, rom.end(), searcher);
    if (found == rom.end()) break;
    const auto position = static_cast<std::size_t>(found - rom.begin());
    if ((position & 1U) == 0) {
      std::size_t length = kMinRun;
      while (offset + length < work_ram.size() && position + length < rom.size() && work_ram[offset + length] == rom[position + length])
        ++length;
      std::size_t back = 0;
      while (back + 2 <= offset && back + 2 <= position && work_ram[offset - back - 2] == rom[position - back - 2] &&
             work_ram[offset - back - 1] == rom[position - back - 1])
        back += 2;
      length -= length & 1U;
      if (!best || back + length > best->total) best = Best{back + length, back, position};
    }
    from = found + 1;  // overlapping occurrences are candidates too
  }
  if (!best) return std::nullopt;
  return CopyAlias{static_cast<std::uint32_t>(kWorkRamBegin + offset - best->back), static_cast<std::uint32_t>(best->position - best->back),
                   static_cast<std::uint32_t>(best->total)};
}

std::vector<CopyAlias> merge_copy_aliases(std::vector<CopyAlias> aliases) {
  std::sort(aliases.begin(), aliases.end());
  std::vector<CopyAlias> merged;
  for (const CopyAlias& alias : aliases) {
    if (!merged.empty()) {
      CopyAlias& last = merged.back();
      const auto delta = [](const CopyAlias& a) { return static_cast<std::int64_t>(a.execution) - static_cast<std::int64_t>(a.source); };
      const std::uint64_t last_end = static_cast<std::uint64_t>(last.execution) + last.length;
      if (delta(alias) == delta(last) && alias.execution <= last_end) {
        const std::uint64_t end = std::max(last_end, static_cast<std::uint64_t>(alias.execution) + alias.length);
        last.length = static_cast<std::uint32_t>(end - last.execution);
        continue;
      }
    }
    merged.push_back(alias);
  }
  return merged;
}

const char* termination_name(Termination termination) noexcept {
  switch (termination) {
    case Termination::route_advanced: return "route_advanced";
    case Termination::non_alias_frontier: return "non_alias_frontier";
    case Termination::no_work_ram_frontier: return "no_work_ram_frontier";
    case Termination::frontier_not_verbatim_copy: return "frontier_not_verbatim_copy";
    case Termination::repeated_alias_no_progress: return "repeated_alias_no_progress";
    case Termination::max_rounds: return "max_rounds";
    case Termination::tool_failure: return "tool_failure";
  }
  return "tool_failure";
}

Step next_step(std::span<const std::uint8_t> rom, const RoundObservation& observed, const std::vector<CopyAlias>& aliases) {
  Step step;
  if (observed.tool_failure) { step.termination = Termination::tool_failure; return step; }
  if (!observed.guest_stop) { step.termination = Termination::route_advanced; return step; }
  if (!observed.record || observed.record->work_ram.size() != kWorkRamBytes) { step.termination = Termination::tool_failure; return step; }
  const GuestStopRecord& record = *observed.record;
  if (record.stop_class != kStopKnownButUnemittedTarget && record.stop_class != kStopInternalDispatchInconsistency) {
    step.termination = Termination::non_alias_frontier;
    return step;
  }
  if ((record.pc & 1U) != 0 || record.pc < kWorkRamBegin || record.pc >= kWorkRamBegin + kWorkRamBytes) {
    step.termination = Termination::no_work_ram_frontier;
    return step;
  }
  const auto proposal = derive_copy_alias(rom, record.work_ram, record.pc);
  if (!proposal) { step.termination = Termination::frontier_not_verbatim_copy; return step; }
  std::vector<CopyAlias> next = aliases;
  next.push_back(*proposal);
  next = merge_copy_aliases(std::move(next));
  if (next == aliases) { step.termination = Termination::repeated_alias_no_progress; return step; }
  step.aliases = std::move(next);
  return step;
}

std::uint64_t Summary::alias_bytes() const noexcept {
  std::uint64_t total = 0;
  for (const CopyAlias& alias : aliases) total += alias.length;
  return total;
}

Summary prepare(std::span<const std::uint8_t> rom, const RoundObservation& initial, RoundRunner& runner, std::size_t max_rounds) {
  Summary summary;
  RoundObservation observed = initial;
  for (;;) {
    Step step = next_step(rom, observed, summary.aliases);
    if (step.termination) { summary.termination = *step.termination; break; }
    if (summary.rounds >= max_rounds) { summary.termination = Termination::max_rounds; break; }
    ++summary.rounds;
    summary.aliases = std::move(step.aliases);
    bool abort = false;
    observed = runner.run_round(summary.aliases, abort);
    if (abort) { summary.aborted = true; summary.termination = Termination::tool_failure; break; }
  }
  return summary;
}

}  // namespace segarecomp::machine::genesis::m68k_alias
