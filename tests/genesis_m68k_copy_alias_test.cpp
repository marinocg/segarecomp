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
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--probe") return probe();
  unit_cases();
  std::printf("genesis m68k copy alias: %s\n", failures == 0 ? "ok" : "FAILED");
  return failures == 0 ? 0 : 1;
}
