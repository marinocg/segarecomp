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
    case Termination::ram_thunk_mismatch: return "ram_thunk_mismatch";
    case Termination::max_rounds: return "max_rounds";
    case Termination::tool_failure: return "tool_failure";
  }
  return "tool_failure";
}

namespace {
// The RAM-thunk decision for a frontier at work-RAM `pc` (a non-copy execution target, or an IRQ6 vector entry).
Step thunk_step(const GuestStopRecord& record, std::uint32_t pc, const std::vector<CopyAlias>& aliases, const std::vector<RamThunk>& thunks,
                const RamThunkRecognizer& recognizer) {
  Step step;
  if ((pc & 1U) != 0 || pc < kWorkRamBegin || pc >= kWorkRamBegin + kWorkRamBytes) { step.termination = Termination::no_work_ram_frontier; return step; }
  const std::size_t offset = pc - kWorkRamBegin;
  const std::size_t window = std::min<std::size_t>(6, record.work_ram.size() - offset);
  // A materialized thunk is a bounded-build-time-materialization image: its bytes are an observed fact. Observing the same address
  // again must show the same bytes (then the stop is simply no progress); different bytes mean the observation is not
  // reproducible, which is a typed, incomplete preparation, never "no progress" and never silently re-materialized.
  for (const RamThunk& existing : thunks) {
    if (pc >= existing.execution && pc < existing.execution + existing.bytes.size()) {  // the stop is inside a materialized stub
      const bool same = existing.execution == pc && window >= existing.bytes.size() &&
                        std::equal(existing.bytes.begin(), existing.bytes.end(), record.work_ram.begin() + static_cast<std::ptrdiff_t>(offset));
      step.termination = same ? Termination::repeated_alias_no_progress : Termination::ram_thunk_mismatch;
      return step;
    }
  }
  const auto bytes = recognizer ? recognizer(pc, std::span<const std::uint8_t>(record.work_ram).subspan(offset, window)) : std::nullopt;
  if (!bytes) { step.termination = Termination::frontier_not_verbatim_copy; return step; }
  for (const RamThunk& existing : thunks)  // the recognized (decoder-trimmed) instruction must not straddle another materialized stub
    if (existing.execution < pc + bytes->size() && pc < existing.execution + existing.bytes.size()) {
      step.termination = Termination::ram_thunk_mismatch;
      return step;
    }
  for (const CopyAlias& alias : aliases)
    if (pc < static_cast<std::uint64_t>(alias.execution) + alias.length && alias.execution < pc + bytes->size()) {
      step.termination = Termination::repeated_alias_no_progress;
      return step;
    }
  step.aliases = aliases;
  step.thunks = thunks;
  step.thunks.push_back({pc, *bytes});
  std::sort(step.thunks.begin(), step.thunks.end(), [](const RamThunk& a, const RamThunk& b) { return a.execution < b.execution; });
  return step;
}
}  // namespace

Step next_step(std::span<const std::uint8_t> rom, const RoundObservation& observed, const std::vector<CopyAlias>& aliases,
               const std::vector<RamThunk>& thunks, const RamThunkRecognizer& recognizer) {
  Step step;
  if (observed.tool_failure) { step.termination = Termination::tool_failure; return step; }
  if (!observed.guest_stop) { step.termination = Termination::route_advanced; return step; }
  if (!observed.record || observed.record->work_ram.size() != kWorkRamBytes) { step.termination = Termination::tool_failure; return step; }
  const GuestStopRecord& record = *observed.record;
  if (record.stop_class == kStopUnsupportedInterrupt) {
    // An IRQ6 delivery through a work-RAM vector: the cartridge's own vector slot names the stub address (immutable data).
    if (rom.size() < kIrq6VectorOffset + 4U) { step.termination = Termination::non_alias_frontier; return step; }
    const std::uint32_t vector = ((static_cast<std::uint32_t>(rom[kIrq6VectorOffset]) << 24) | (static_cast<std::uint32_t>(rom[kIrq6VectorOffset + 1U]) << 16) |
                                  (static_cast<std::uint32_t>(rom[kIrq6VectorOffset + 2U]) << 8) | rom[kIrq6VectorOffset + 3U]) & 0x00FFFFFFU;
    if (vector < kWorkRamBegin) { step.termination = Termination::non_alias_frontier; return step; }
    return thunk_step(record, vector, aliases, thunks, recognizer);
  }
  if (record.stop_class != kStopKnownButUnemittedTarget && record.stop_class != kStopInternalDispatchInconsistency) {
    step.termination = Termination::non_alias_frontier;
    return step;
  }
  if ((record.pc & 1U) != 0 || record.pc < kWorkRamBegin || record.pc >= kWorkRamBegin + kWorkRamBytes) {
    step.termination = Termination::no_work_ram_frontier;
    return step;
  }
  const auto proposal = derive_copy_alias(rom, record.work_ram, record.pc);
  if (!proposal) return thunk_step(record, record.pc, aliases, thunks, recognizer);  // not a verbatim copy: maybe one JMP thunk
  std::vector<CopyAlias> next = aliases;
  next.push_back(*proposal);
  next = merge_copy_aliases(std::move(next));
  if (next == aliases) { step.termination = Termination::repeated_alias_no_progress; return step; }
  step.aliases = std::move(next);
  step.thunks = thunks;
  return step;
}

std::uint64_t Summary::alias_bytes() const noexcept {
  std::uint64_t total = 0;
  for (const CopyAlias& alias : aliases) total += alias.length;
  return total;
}

Summary prepare(std::span<const std::uint8_t> rom, const RoundObservation& initial, RoundRunner& runner, std::size_t max_rounds,
                const RamThunkRecognizer& recognizer) {
  Summary summary;
  RoundObservation observed = initial;
  for (;;) {
    Step step = next_step(rom, observed, summary.aliases, summary.thunks, recognizer);
    if (step.termination) { summary.termination = *step.termination; break; }
    if (summary.rounds >= max_rounds) { summary.termination = Termination::max_rounds; break; }
    ++summary.rounds;
    summary.aliases = std::move(step.aliases);
    summary.thunks = std::move(step.thunks);
    bool abort = false;
    observed = runner.run_round(summary.aliases, summary.thunks, abort);
    if (abort) { summary.aborted = true; summary.termination = Termination::tool_failure; break; }
  }
  return summary;
}

}  // namespace segarecomp::machine::genesis::m68k_alias
