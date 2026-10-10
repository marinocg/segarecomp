// SEG-028-T005 (ADR 0077, ADR 0049): the consumer-route alias preparation library. Synthetic bytes only.
//
//   genesis_m68k_copy_alias_test            unit cases: stop-record layout, derivation, merge, the decision step and the bounded loop
//                                           (every termination, including max_rounds and tool_failure, with a scripted round runner)
//   genesis_m68k_copy_alias_test --probe    parity probe for tests/genesis_m68k_copy_alias_parity_test.py; reads one case per stdin
//                                           line and prints one result line:
//                                             derive <rom-file> <ram-file> <pc-hex>  ->  none | <exec-hex> <source-hex> <length-hex>
//                                             merge [<exec>:<source>:<length> (hex)]...  ->  the merged list in the same form
#include "segarecomp/machine/genesis/m68k_copy_alias.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace ma = segarecomp::machine::genesis::m68k_alias;

namespace {

int failures = 0;

void check(bool ok, const char* label) {
  std::printf("%-5s %s\n", ok ? "ok" : "FAIL", label);
  if (!ok) ++failures;
}

std::vector<std::uint8_t> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string format(const ma::CopyAlias& alias) {
  char text[40];
  std::snprintf(text, sizeof(text), "%x:%x:%x", static_cast<unsigned>(alias.execution), static_cast<unsigned>(alias.source),
                static_cast<unsigned>(alias.length));
  return text;
}

int probe() {
  for (std::string line; std::getline(std::cin, line);) {
    std::istringstream in(line);
    std::string verb;
    in >> verb;
    if (verb == "derive") {
      std::string rom_path, ram_path, pc_text;
      in >> rom_path >> ram_path >> pc_text;
      const auto rom = read_file(rom_path);
      const auto ram = read_file(ram_path);
      const auto alias = ma::derive_copy_alias(rom, ram, static_cast<std::uint32_t>(std::stoul(pc_text, nullptr, 16)));
      std::cout << (alias ? format(*alias) : std::string("none")) << '\n';
    } else if (verb == "merge") {
      std::vector<ma::CopyAlias> aliases;
      for (std::string item; in >> item;) {
        const auto first = item.find(':'), second = item.find(':', first + 1);
        aliases.push_back({static_cast<std::uint32_t>(std::stoul(item.substr(0, first), nullptr, 16)),
                           static_cast<std::uint32_t>(std::stoul(item.substr(first + 1, second - first - 1), nullptr, 16)),
                           static_cast<std::uint32_t>(std::stoul(item.substr(second + 1), nullptr, 16))});
      }
      std::string out;
      for (const auto& alias : ma::merge_copy_aliases(aliases)) out += (out.empty() ? "" : " ") + format(alias);
      std::cout << out << '\n';
    } else {
      return 2;
    }
  }
  return 0;
}

// A synthetic image with a recognizable 48-byte run at an even offset, and work RAM holding it at `ram_offset`.
struct Fixture {
  std::vector<std::uint8_t> rom = std::vector<std::uint8_t>(0x1000, 0xEE);
  std::vector<std::uint8_t> ram = std::vector<std::uint8_t>(ma::kWorkRamBytes, 0x00);
  Fixture(std::uint32_t rom_offset, std::uint32_t ram_offset, std::uint32_t length) {
    for (std::uint32_t i = 0; i < length; ++i) {
      rom[rom_offset + i] = static_cast<std::uint8_t>(0x10 + i * 7);
      ram[ram_offset + i] = rom[rom_offset + i];
    }
  }
};

ma::RoundObservation stop_at(const std::vector<std::uint8_t>& ram, std::uint32_t pc, std::uint32_t stop_class = ma::kStopKnownButUnemittedTarget) {
  ma::RoundObservation observed;
  observed.guest_stop = true;
  observed.record = ma::GuestStopRecord{pc, stop_class, ram};
  return observed;
}

class Scripted final : public ma::RoundRunner {
 public:
  std::vector<ma::RoundObservation> script;
  std::vector<std::vector<ma::CopyAlias>> seen;
  bool abort_at_end = false;
  ma::RoundObservation run_round(const std::vector<ma::CopyAlias>& aliases, bool& abort) override {
    seen.push_back(aliases);
    if (seen.size() > script.size()) {
      abort = abort_at_end;
      return {};
    }
    return script[seen.size() - 1];
  }
};

void unit_cases() {
  // Stop record layout: 4-byte BE pc, 4-byte BE class, 64 KiB work RAM.
  std::vector<std::uint8_t> bytes(ma::kStopRecordBytes, 0);
  bytes[0] = 0x00; bytes[1] = 0xFF; bytes[2] = 0x12; bytes[3] = 0x34; bytes[7] = 5; bytes[8] = 0xAB;
  const auto record = ma::parse_stop_record(bytes);
  check(record && record->pc == 0xFF1234 && record->stop_class == 5 && record->work_ram.size() == ma::kWorkRamBytes && record->work_ram[0] == 0xAB,
        "stop record: big-endian pc and class, then the work RAM");
  bytes.pop_back();
  check(!ma::parse_stop_record(bytes), "stop record: a short record is rejected");

  Fixture f(0x200, 0x400, 48);
  const auto alias = ma::derive_copy_alias(f.rom, f.ram, 0xFF0400);
  check(alias && *alias == ma::CopyAlias{0xFF0400, 0x200, 48}, "derive: the verbatim run at the stop pc");
  const auto inside = ma::derive_copy_alias(f.rom, f.ram, 0xFF0410);
  check(inside && *inside == ma::CopyAlias{0xFF0400, 0x200, 48}, "derive: back-extension by words to the start of the run");
  check(!ma::derive_copy_alias(f.rom, f.ram, 0xFF0401), "derive: odd pc rejected");
  check(!ma::derive_copy_alias(f.rom, f.ram, 0x000400), "derive: non-work-RAM pc rejected");
  check(!ma::derive_copy_alias(f.rom, f.ram, 0xFF0428), "derive: fewer than the minimum run bytes left in the run rejected");
  Fixture odd(0x201, 0x400, 48);
  check(!ma::derive_copy_alias(odd.rom, odd.ram, 0xFF0400), "derive: only odd ROM occurrences: rejected");

  // The decision step.
  const std::vector<ma::CopyAlias> none;
  ma::RoundObservation advanced;
  check(ma::next_step(f.rom, advanced, none).termination == ma::Termination::route_advanced, "step: no guest stop -> route_advanced");
  check(ma::next_step(f.rom, stop_at(f.ram, 0xFF0400, 3), none).termination == ma::Termination::non_alias_frontier, "step: other stop class -> non_alias_frontier");
  check(ma::next_step(f.rom, stop_at(f.ram, 0x000400), none).termination == ma::Termination::no_work_ram_frontier, "step: ROM pc -> no_work_ram_frontier");
  check(ma::next_step(f.rom, stop_at(f.ram, 0xFF0402, ma::kStopInternalDispatchInconsistency), none).aliases ==
            std::vector<ma::CopyAlias>{{0xFF0400, 0x200, 48}}, "step: internal_dispatch_inconsistency is an alias class");
  check(ma::next_step(f.rom, stop_at(f.ram, 0xFF0800), none).termination == ma::Termination::frontier_not_verbatim_copy, "step: no run -> frontier_not_verbatim_copy");
  const auto first = ma::next_step(f.rom, stop_at(f.ram, 0xFF0400), none);
  check(!first.termination && first.aliases == std::vector<ma::CopyAlias>{{0xFF0400, 0x200, 48}}, "step: a verbatim copy -> one alias");
  check(ma::next_step(f.rom, stop_at(f.ram, 0xFF0410), first.aliases).termination == ma::Termination::repeated_alias_no_progress,
        "step: an already covered proposal -> repeated_alias_no_progress");
  // ADR 0097 amendment: the RAM-thunk decision. The recognizer is injected (the library never knows an opcode); this fake accepts the
  // 4-byte pattern AA BB CC DD only.
  const ma::RamThunkRecognizer fake = [](std::uint32_t, std::span<const std::uint8_t> window) -> std::optional<std::vector<std::uint8_t>> {
    if (window.size() >= 4 && window[0] == 0xAA && window[1] == 0xBB && window[2] == 0xCC && window[3] == 0xDD)
      return std::vector<std::uint8_t>(window.begin(), window.begin() + 4);
    return std::nullopt;
  };
  {
    Fixture t(0x200, 0x400, 48);
    t.ram[0x0800] = 0xAA; t.ram[0x0801] = 0xBB; t.ram[0x0802] = 0xCC; t.ram[0x0803] = 0xDD;
    // not a verbatim copy and a recognised stub: one thunk (no recognizer: the old fail-closed termination)
    check(ma::next_step(t.rom, stop_at(t.ram, 0xFF0800), none).termination == ma::Termination::frontier_not_verbatim_copy,
          "thunk: without a recognizer nothing is proposed");
    const auto proposed = ma::next_step(t.rom, stop_at(t.ram, 0xFF0800), none, {}, fake);
    check(!proposed.termination && proposed.thunks.size() == 1 && proposed.thunks[0].execution == 0xFF0800 &&
              proposed.thunks[0].bytes == std::vector<std::uint8_t>{0xAA, 0xBB, 0xCC, 0xDD},
          "thunk: a recognised non-copy frontier proposes exactly the recognised bytes");
    check(ma::next_step(t.rom, stop_at(t.ram, 0xFF0802), none, {}, fake).termination == ma::Termination::frontier_not_verbatim_copy,
          "thunk: a frontier inside a stub (not its first byte) is not recognised");
    check(ma::next_step(t.rom, stop_at(t.ram, 0xFF0800), none, proposed.thunks, fake).termination == ma::Termination::repeated_alias_no_progress,
          "thunk: the same bytes observed again at a materialized stub are no progress (deterministic)");
    // Reproducibility: the materialized image is an observed fact. Different live bytes at its address are a typed, INCOMPLETE
    // preparation (never no-progress, never re-materialized), whether the new bytes are another JMP or not a JMP at all.
    auto changed = t.ram;
    changed[0x0800] = 0xAA; changed[0x0801] = 0xBB; changed[0x0802] = 0xCC; changed[0x0803] = 0xEE;  // a different recognized stub
    const auto mismatch = ma::next_step(t.rom, stop_at(changed, 0xFF0800), none, proposed.thunks, fake);
    check(mismatch.termination == ma::Termination::ram_thunk_mismatch && ma::incomplete(*mismatch.termination) && mismatch.thunks.empty(),
          "thunk: changed bytes at a materialized address -> ram_thunk_mismatch (incomplete), nothing re-materialized");
    changed[0x0800] = 0x00;  // no longer a stub at all
    check(ma::next_step(t.rom, stop_at(changed, 0xFF0800), none, proposed.thunks, fake).termination == ma::Termination::ram_thunk_mismatch,
          "thunk: bytes that are no longer a stub at a materialized address -> ram_thunk_mismatch");
    check(ma::next_step(t.rom, stop_at(changed, 0xFF0802), none, proposed.thunks, fake).termination == ma::Termination::ram_thunk_mismatch,
          "thunk: a stop inside a materialized stub whose bytes changed -> ram_thunk_mismatch");
    check(ma::next_step(t.rom, stop_at(t.ram, 0xFF0802), none, proposed.thunks, fake).termination == ma::Termination::ram_thunk_mismatch,
          "thunk: a stop inside a materialized stub is never explained by it -> ram_thunk_mismatch");
    check(ma::next_step(t.rom, stop_at(t.ram, 0xFF0400), none, {}, fake).aliases == std::vector<ma::CopyAlias>{{0xFF0400, 0x200, 48}},
          "thunk: a verbatim copy keeps priority over the thunk path");
    // IRQ6 through a work-RAM vector: the cartridge's own vector slot names the stub address
    t.rom[0x78] = 0x00; t.rom[0x79] = 0xFF; t.rom[0x7A] = 0x08; t.rom[0x7B] = 0x00;
    const auto irq = ma::next_step(t.rom, stop_at(t.ram, 0xFF0123, ma::kStopUnsupportedInterrupt), none, {}, fake);
    check(!irq.termination && irq.thunks.size() == 1 && irq.thunks[0].execution == 0xFF0800, "thunk: an IRQ6 stop proposes the stub the vector names");
    t.rom[0x78] = 0x00; t.rom[0x79] = 0x00; t.rom[0x7A] = 0x02; t.rom[0x7B] = 0x00;
    check(ma::next_step(t.rom, stop_at(t.ram, 0xFF0123, ma::kStopUnsupportedInterrupt), none, {}, fake).termination ==
              ma::Termination::non_alias_frontier, "thunk: an IRQ6 stop whose vector is in ROM is not ours");
    // the loop threads the thunk set through the runner and terminates on no progress
    struct Thunks final : ma::RoundRunner {
      std::vector<std::vector<ma::RamThunk>> seen;
      ma::RoundObservation next;
      ma::RoundObservation run_round(const std::vector<ma::CopyAlias>&, bool&) override { return {}; }
      ma::RoundObservation run_round(const std::vector<ma::CopyAlias>&, const std::vector<ma::RamThunk>& thunks, bool&) override {
        seen.push_back(thunks);
        return next;
      }
    } runner;
    runner.next = stop_at(t.ram, 0xFF0800);
    const auto summary = ma::prepare(t.rom, stop_at(t.ram, 0xFF0800), runner, ma::kMaxRounds, fake);
    check(summary.termination == ma::Termination::repeated_alias_no_progress && summary.rounds == 1 && summary.thunks.size() == 1 &&
              runner.seen.size() == 1 && runner.seen[0].size() == 1,
          "thunk: the bounded loop builds the thunk once, then stops on no progress");
    // Adjacent but distinct stubs are not a mismatch: a stub already materialized right after this one (inside the 6-byte probe
    // window, outside the decoder-trimmed instruction) leaves the new proposal valid; only a recognized instruction that really
    // straddles a materialized stub is.
    {
      Fixture adj(0x200, 0x400, 48);
      for (std::size_t i = 0; i < 4; ++i) { adj.ram[0x0800 + i] = proposed.thunks[0].bytes[i]; adj.ram[0x0804 + i] = static_cast<std::uint8_t>(0x10 + i); }
      const std::vector<ma::RamThunk> later{{0xFF0804, {0x10, 0x11, 0x12, 0x13}}};
      const auto next = ma::next_step(adj.rom, stop_at(adj.ram, 0xFF0800), none, later, fake);
      check(!next.termination && next.thunks.size() == 2 && next.thunks[0].execution == 0xFF0800 && next.thunks[1].execution == 0xFF0804,
            "thunk: an adjacent, distinct materialized stub is not a mismatch");
      adj.ram[0x0802] = 0xAA; adj.ram[0x0803] = 0xBB; adj.ram[0x0804] = 0xCC; adj.ram[0x0805] = 0xDD;  // recognized at 0x802, straddles 0x804
      const std::vector<ma::RamThunk> straddled{{0xFF0804, {0xCC, 0xDD, 0x00, 0x00}}};
      check(ma::next_step(adj.rom, stop_at(adj.ram, 0xFF0802), none, straddled, fake).termination == ma::Termination::ram_thunk_mismatch,
            "thunk: a recognized instruction straddling a materialized stub -> ram_thunk_mismatch");
    }
    // The loop: a later observation of the same address with different bytes ends the preparation INCOMPLETE; the changed target is
    // never proposed or built (the runner saw exactly one thunk set, the original one).
    runner.seen.clear();
    runner.next = stop_at(changed, 0xFF0800);
    const auto bad = ma::prepare(t.rom, stop_at(t.ram, 0xFF0800), runner, ma::kMaxRounds, fake);
    check(bad.termination == ma::Termination::ram_thunk_mismatch && ma::incomplete(bad.termination) && bad.rounds == 1 &&
              bad.thunks.size() == 1 && bad.thunks[0].bytes == std::vector<std::uint8_t>{0xAA, 0xBB, 0xCC, 0xDD} && runner.seen.size() == 1,
          "thunk: the loop ends ram_thunk_mismatch (incomplete) with the original bytes only");
  }
  ma::RoundObservation missing;
  missing.guest_stop = true;
  check(ma::next_step(f.rom, missing, none).termination == ma::Termination::tool_failure, "step: guest stop without a record -> tool_failure");

  // The bounded loop.
  {
    Scripted runner;
    runner.script = {advanced};
    const auto summary = ma::prepare(f.rom, stop_at(f.ram, 0xFF0400), runner);
    check(summary.termination == ma::Termination::route_advanced && summary.rounds == 1 && summary.aliases.size() == 1 && summary.alias_bytes() == 48 &&
              runner.seen.size() == 1, "loop: one alias, then the route advances");
  }
  {
    Scripted runner;
    const auto summary = ma::prepare(f.rom, advanced, runner);
    check(summary.termination == ma::Termination::route_advanced && summary.rounds == 0 && runner.seen.empty(), "loop: no stop -> zero rounds");
  }
  {
    Scripted runner;
    runner.script = {stop_at(f.ram, 0xFF0420)};  // inside the alias just added: no progress
    const auto summary = ma::prepare(f.rom, stop_at(f.ram, 0xFF0400), runner);
    check(summary.termination == ma::Termination::repeated_alias_no_progress && summary.rounds == 1 && summary.aliases.size() == 1,
          "loop: a repeated proposal stops with the aliases found");
  }
  {
    // Two disjoint copies discovered one after the other, then a cap of 1 round: max_rounds.
    Fixture two(0x200, 0x400, 48);
    for (std::uint32_t i = 0; i < 32; ++i) {
      two.rom[0x800 + i] = static_cast<std::uint8_t>(0x90 + i * 3);
      two.ram[0x2000 + i] = two.rom[0x800 + i];
    }
    Scripted runner;
    runner.script = {stop_at(two.ram, 0xFF2000), advanced};
    const auto full = ma::prepare(two.rom, stop_at(two.ram, 0xFF0400), runner);
    check(full.termination == ma::Termination::route_advanced && full.rounds == 2 && full.aliases.size() == 2 && full.alias_bytes() == 80,
          "loop: two copies -> two aliases (sorted), route advanced");
    Scripted capped;
    capped.script = {stop_at(two.ram, 0xFF2000)};
    const auto incomplete = ma::prepare(two.rom, stop_at(two.ram, 0xFF0400), capped, 1);
    check(incomplete.termination == ma::Termination::max_rounds && ma::incomplete(incomplete.termination) && incomplete.rounds == 1,
          "loop: still discovering at the bound -> max_rounds (incomplete)");
  }
  {
    Scripted runner;
    runner.script = {ma::RoundObservation{true, false, std::nullopt}};
    const auto summary = ma::prepare(f.rom, stop_at(f.ram, 0xFF0400), runner);
    check(summary.termination == ma::Termination::tool_failure && ma::incomplete(summary.termination) && !summary.aborted,
          "loop: a round that cannot be built -> tool_failure (incomplete)");
    Scripted aborting;
    aborting.abort_at_end = true;
    const auto aborted = ma::prepare(f.rom, stop_at(f.ram, 0xFF0400), aborting);
    check(aborted.aborted && aborted.rounds == 1, "loop: a round whose fixed point fails aborts with the runner's failure");
  }
  for (const auto t : {ma::Termination::route_advanced, ma::Termination::non_alias_frontier, ma::Termination::no_work_ram_frontier,
                       ma::Termination::frontier_not_verbatim_copy, ma::Termination::repeated_alias_no_progress})
    check(!ma::incomplete(t), ma::termination_name(t));
  check(ma::incomplete(ma::Termination::ram_thunk_mismatch) && std::string(ma::termination_name(ma::Termination::ram_thunk_mismatch)) ==
                                                                  "ram_thunk_mismatch",
        "ram_thunk_mismatch is an incomplete termination with its own name");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--probe") return probe();
  unit_cases();
  std::printf("genesis m68k copy alias: %s\n", failures == 0 ? "ok" : "FAILED");
  return failures == 0 ? 0 : 1;
}
