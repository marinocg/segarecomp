// SEG-097-T004 (ADR 0209): does the source-relative mechanism of ADR 0207 also serve the Genesis verbatim ROM-to-work-RAM copy of ADR 0049?
//
// This tool runs the UNCHANGED production Genesis emission on a project-authored fixture (the SEG-021-T041 image: a ROM routine copied verbatim to work
// RAM and called there; it contains a PC-relative read, a BSR, a relative branch, an absolute store and RTS) with the routine's alias at 1..4 work-RAM
// bases, then asks the SHARED derivation (libs/codegen/c11/source_relative.hpp, the same code the Sega CD emitter uses) whether the ROM identity's body and
// the alias identities' bodies of each instruction are one source-relative body. The production emission is never modified.
//
//   --ref K       the production program with K aliases (the reference)
//   --shared K    the same program in which every instruction of the routine is ONE source-relative function serving its ROM address and every alias
//                 address (the byte-identity guard of an alias is conditional on the work-RAM address); the compiled-entry rows point at it
//   --naive K     the ADR 0049 falsification control: the alias addresses are served by the ROM-anchored bodies (what a blind share would do)
//   --report K    JSON: derivation outcome per instruction, an exact textual equivalence check of the shared body against every real lowering, and sizes
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "segarecomp/codegen/c11/genesis_frontend.hpp"
#include "segarecomp/codegen/c11/source_relative.hpp"
#include "segarecomp/machine/genesis/frontend.hpp"

namespace {
using namespace segarecomp;

constexpr std::uint32_t kBase = 0x00000E00U, kSourceOffset = 0x40U, kRoutineLength = 0x28U;
constexpr std::uint32_t kAliasBases[4] = {0x00FF0400U, 0x00FF0500U, 0x00FF0600U, 0x00FF0700U};
const std::vector<std::uint8_t> kImage{
    0x4E, 0xB9, 0x00, 0x00, 0x0E, 0x0E, 0x30, 0x51, 0x4E, 0x90, 0x4E, 0x71, 0x60, 0xF2, 0x4E, 0x75,
    0x41, 0xFA, 0x00, 0x2E, 0x43, 0xF9, 0x00, 0xFF, 0x04, 0x00, 0x74, 0x09, 0x22, 0xD8, 0x51, 0xCA, 0xFF, 0xFC,
    0x4E, 0xB9, 0x00, 0xFF, 0x04, 0x00, 0x4E, 0x71, 0x60, 0xFE,
    0x4E, 0x71, 0x4E, 0x71, 0x4E, 0x71, 0x4E, 0x71, 0x4E, 0x71, 0x4E, 0x71, 0x4E, 0x71, 0x4E, 0x71, 0x4E, 0x71, 0x4E, 0x71,
    // routine, source +0x40
    0x70, 0x05, 0x61, 0x18, 0x4A, 0x40, 0x66, 0x04, 0x7C, 0x63, 0x4E, 0x71, 0x30, 0x3A, 0x00, 0x16, 0x33, 0xC0, 0x00, 0xFF, 0x08, 0x00,
    0x4E, 0x75, 0x4E, 0x71, 0x4E, 0x71, 0x52, 0x40, 0x4E, 0x75, 0x4E, 0x71, 0x4E, 0x71, 0x12, 0x34, 0x00, 0x00};

std::string emit_program(unsigned aliases) {
  FrontendProgram program{};
  program.profile = M68kFrontendProfile::general_startup;
  program.image = {"synthetic/SEG-097-T004/copy-alias-reuse", kImage, kImage.size()};
  program.mapping_claims = {{"raw_cartridge_rom", {{}, kBase}, {{}, static_cast<std::uint32_t>(kBase + kImage.size())}, {0U}, {kImage.size()}}};
  program.startup_ingress = M68kStartupIngress{{{}, kBase}, 0x00FF1000U};
  if (!apply_genesis_immutable_rom_aot(program)) return {};
  for (unsigned i = 0; i < aliases; ++i)
    if (!apply_genesis_immutable_copy_alias(program, kAliasBases[i], kBase + kSourceOffset, kRoutineLength)) return {};
  const auto result = analyze_m68k_frontend(program);
  const auto *partial = std::get_if<FrontendPartialProgram>(&result);
  if (partial == nullptr) return {};
  return emit_m68k_general_startup_runtime_c(*partial);
}

std::string hex8(std::uint32_t v) {
  char buffer[16];
  std::snprintf(buffer, sizeof buffer, "%08X", static_cast<unsigned>(v));
  return buffer;
}

struct Function {
  std::size_t begin = 0, end = 0;  // [begin, end) of the whole definition in the program text
  std::string header, body;        // header = the first line, body = between "{\n" and "\n}\n"
};

std::map<std::uint32_t, Function> parse_functions(const std::string &text) {
  std::map<std::uint32_t, Function> out;
  const std::regex head(R"(static GenesisControlTransfer genesis_aot_([0-9A-F]{8})\(GenesisRuntime \*runtime\) \{\n)");
  for (std::sregex_iterator it(text.begin(), text.end(), head), end; it != end; ++it) {
    Function f;
    f.begin = static_cast<std::size_t>(it->position());
    const std::size_t body_begin = f.begin + static_cast<std::size_t>(it->length());
    // the matching closing brace (the generated code has balanced braces; a lowering may print a block whose brace starts a line)
    std::size_t close = std::string::npos;
    for (std::size_t at = body_begin, depth = 1U; at < text.size(); ++at) {
      if (text[at] == '{') ++depth;
      else if (text[at] == '}' && --depth == 0U) { close = at; break; }
    }
    if (close == std::string::npos || close == 0U || text[close - 1U] != '\n') continue;
    --close;  // the body ends before the newline that precedes the closing brace
    f.end = close + 3U;
    f.header = it->str();
    f.body = text.substr(body_begin, close - body_begin);
    out.emplace(static_cast<std::uint32_t>(std::stoul((*it)[1], nullptr, 16)), std::move(f));
  }
  return out;
}

// The alias identities' byte-identity guard is one physical line starting with this prefix.
const std::string kGuardPrefix = "  if (runtime->work_ram[";
bool take_line(std::string &body, const std::string &prefix, std::string &line) {
  const auto at = body.find("\n" + prefix);
  std::size_t begin;
  if (body.compare(0, prefix.size(), prefix) == 0) begin = 0;
  else if (at != std::string::npos) begin = at + 1U;
  else return false;
  const auto end = body.find('\n', begin);
  line = body.substr(begin, (end == std::string::npos ? body.size() : end) - begin);
  body.erase(begin, (end == std::string::npos ? body.size() : end + 1U) - begin);
  return true;
}

// pc0-expressions back to the literal they evaluate to at `pc` (the inverse of the derivation, to compare with a real lowering).
std::string instantiate(const std::string &text, std::uint32_t pc, std::string_view symbol) {
  const std::regex expr("\\(uint32_t\\)\\(" + std::string(symbol) + " \\+ UINT32_C\\(0x([0-9A-F]{8})\\)\\)");
  std::string out;
  std::size_t last = 0;
  for (std::sregex_iterator it(text.begin(), text.end(), expr), end; it != end; ++it) {
    out.append(text, last, static_cast<std::size_t>(it->position()) - last);
    const auto delta = static_cast<std::uint32_t>(std::stoul((*it)[1], nullptr, 16));
    out += "UINT32_C(0x" + hex8(pc + delta) + ")";
    last = static_cast<std::size_t>(it->position() + it->length());
  }
  out.append(text, last, std::string::npos);
  return out;
}

// Guard offsets are written in decimal by the production lowering and in hexadecimal by the derivation: compare the numbers, not the spelling.
std::string canon_guard(const std::string &text) {
  const std::regex index(R"(work_ram\[UINT32_C\((0x[0-9A-Fa-f]+|[0-9]+)\)\])");
  std::string out;
  std::size_t last = 0;
  for (std::sregex_iterator it(text.begin(), text.end(), index), end; it != end; ++it) {
    out.append(text, last, static_cast<std::size_t>(it->position()) - last);
    const std::string literal = (*it)[1];
    out += "work_ram[" + std::to_string(std::stoul(literal, nullptr, 0)) + "]";
    last = static_cast<std::size_t>(it->position() + it->length());
  }
  out.append(text, last, std::string::npos);
  return out;
}

struct Derived {
  bool ok = false;
  std::string reason;
  std::string provenance, guard, body;  // derived texts (with the pc0 symbol); provenance/guard may be empty
  std::string guard_alias_form;
};

// One instruction: the ROM identity (rom_pc) and the first three alias identities.
Derived derive_instruction(const std::map<std::uint32_t, Function> &fns, std::uint32_t rom_pc, bool with_rom, const std::uint32_t alias_pc[3], unsigned alias_count) {
  Derived d;
  if (alias_count < 3U) { d.reason = "needs_three_aliases"; return d; }
  std::string rom = with_rom ? fns.at(rom_pc).body : std::string();
  std::string alias[3] = {fns.at(alias_pc[0]).body, fns.at(alias_pc[1]).body, fns.at(alias_pc[2]).body};
  std::string guards[3];
  for (int i = 0; i < 3; ++i)
    if (!take_line(alias[i], kGuardPrefix, guards[i])) { d.reason = "alias_without_guard"; return d; }
  // provenance line (the source literal moves with the PC)
  const std::string prov = "  const GenesisInstructionProvenance *const genesis_aot_source";
  std::string prov_rom, prov_alias[3];
  const bool rom_has = with_rom && take_line(rom, prov, prov_rom);
  bool all_alias = true;
  for (int i = 0; i < 3; ++i) all_alias = take_line(alias[i], prov, prov_alias[i]) && all_alias;
  if (!all_alias) { d.reason = "alias_without_provenance"; return d; }
  const std::array<std::uint32_t, 3> pcs = {alias_pc[0], alias_pc[1], alias_pc[2]};
  const auto g = derive_source_relative_text({guards[0], guards[1], guards[2]}, pcs, "genesis_pc0");
  if (g.status != SourceRelativeStatus::ok) { d.reason = std::string("guard_") + source_relative_status_name(g.status); return d; }
  d.guard = g.text;
  if (rom_has) {
    // the ROM identity's provenance is the same family: derive over (rom, alias0, alias1)
    const std::array<std::uint32_t, 3> p3 = {rom_pc, alias_pc[0], alias_pc[1]};
    const auto p = derive_source_relative_text({prov_rom, prov_alias[0], prov_alias[1]}, p3, "genesis_pc0");
    if (p.status != SourceRelativeStatus::ok) { d.reason = std::string("provenance_") + source_relative_status_name(p.status); return d; }
    d.provenance = p.text;
  } else {
    const auto p = derive_source_relative_text({prov_alias[0], prov_alias[1], prov_alias[2]}, pcs, "genesis_pc0");
    if (p.status != SourceRelativeStatus::ok) { d.reason = std::string("provenance_") + source_relative_status_name(p.status); return d; }
    d.provenance = p.text;
  }
  // the body: over (ROM, alias0, alias1) when the ROM identity is a full function, else over the three aliases (the ROM identity keeps its own entry)
  const std::array<std::uint32_t, 3> p3 = with_rom ? std::array<std::uint32_t, 3>{rom_pc, alias_pc[0], alias_pc[1]} : pcs;
  const auto b = with_rom ? derive_source_relative_text({rom, alias[0], alias[1]}, p3, "genesis_pc0")
                          : derive_source_relative_text({alias[0], alias[1], alias[2]}, p3, "genesis_pc0");
  if (b.status != SourceRelativeStatus::ok) { d.reason = std::string("body_") + source_relative_status_name(b.status); return d; }
  d.body = b.text;
  d.ok = true;
  return d;
}

std::string shared_function(const std::string &name, const Derived &d) {
  std::string out = "static GenesisControlTransfer " + name + "(GenesisRuntime *runtime) {\n";
  out += "  const uint32_t genesis_pc0 = runtime->pc; /* the guest address this execution was dispatched at (ADR 0207/0209) */\n";
  out += d.provenance + "\n";
  // `uint32_t pc = runtime->pc;` is the first line of the derived body; keep it, then capture the dispatch address
  const std::string pc_line = "  uint32_t pc = runtime->pc;\n";
  std::string body = d.body;
  const auto at = body.find(pc_line);
  if (at == std::string::npos) return {};
  body.insert(at + pc_line.size(), "  if (genesis_pc0 >= UINT32_C(0x00FF0000)) {\n  " + d.guard + "\n  }\n");
  out += body + "\n}\n";
  return out;
}

struct Plan {
  std::string program;
  unsigned shared = 0, fallbacks = 0, instructions = 0;
  std::vector<std::string> reasons;
  std::vector<std::string> unverified;
};

}  // namespace

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  const std::string mode = argv[1];
  const unsigned k = static_cast<unsigned>(std::stoul(argv[2]));
  if (k < 1U || k > 4U) return 2;
  const std::string reference = emit_program(k);
  if (reference.empty()) return 3;
  if (mode == "--ref") { std::cout << reference; return 0; }
  auto fns = parse_functions(reference);
  // the instructions of the routine: those with a ROM function and an alias function at every alias base
  // The routine's instructions: those with a full alias function at every alias base. The ROM identity is shared too when it is a full function as well;
  // some ROM identities are one-line wrappers of the production body-sharing helpers (SEG-022-T011) and keep their own entry.
  std::vector<std::uint32_t> offsets;
  for (std::uint32_t o = 0; o < kRoutineLength; o += 2U) {
    bool all = true;
    for (unsigned i = 0; i < k; ++i) all = all && fns.count(kAliasBases[i] + o) != 0U;
    if (all) offsets.push_back(o);
  }
  if (mode == "--naive") {
    std::string text = reference;
    // serve every alias address by the ROM-anchored body: remove the alias definitions, rename their uses
    std::vector<std::pair<std::size_t, std::size_t>> cuts;
    for (const auto o : offsets) {
      if (fns.count(kBase + kSourceOffset + o) == 0U) continue;
      for (unsigned i = 0; i < k; ++i) cuts.emplace_back(fns.at(kAliasBases[i] + o).begin, fns.at(kAliasBases[i] + o).end);
    }
    std::sort(cuts.rbegin(), cuts.rend());
    for (const auto &[b, e] : cuts) text.erase(b, e - b);
    for (const auto o : offsets)
      for (unsigned i = 0; i < k && fns.count(kBase + kSourceOffset + o) != 0U; ++i) {
        const std::string from = "genesis_aot_" + hex8(kAliasBases[i] + o), to = "genesis_aot_" + hex8(kBase + kSourceOffset + o);
        for (std::size_t at; (at = text.find(from)) != std::string::npos;) text.replace(at, from.size(), to);
      }
    std::cout << text;
    return 0;
  }
  // The derivation needs three distinct lowerings besides the ROM identity's; it always uses the first three alias bases of an auxiliary program, so
  // any K (even 1) is served; the exact equivalence below is always checked against the real program's own lowering, and for K = 4 the fourth base is a held-out check.
  // With K >= 3 the program derives from its own lowerings (the program-wide authority sets embedded in some bodies, e.g. the RTS return-target list, are then the
  // same text everywhere); with K < 3 an auxiliary three-alias program supplies the three points, and every derived body is then checked against the real K-alias
  // program's own lowering - an instruction whose body depends on the whole-program set simply fails that check and keeps its own function.
  const std::string auxiliary = k >= 3U ? reference : emit_program(3);
  if (auxiliary.empty()) return 3;
  const auto aux_fns = parse_functions(auxiliary);
  const auto has_rom = [&](std::uint32_t o) { return aux_fns.count(kBase + kSourceOffset + o) != 0U && fns.count(kBase + kSourceOffset + o) != 0U; };
  std::uint32_t alias_pc[3] = {0, 0, 0};
  Plan plan;
  std::ostringstream report;
  std::string text = reference;
  std::vector<std::pair<std::size_t, std::size_t>> cuts;
  std::string definitions;
  std::map<std::string, std::string> renames;
  struct Row { std::uint32_t offset; bool ok; std::string reason; bool equivalent; bool with_rom; };
  std::vector<Row> rows;
  for (const auto o : offsets) {
    const std::uint32_t rom_pc = kBase + kSourceOffset + o;
    for (unsigned i = 0; i < 3U; ++i) alias_pc[i] = kAliasBases[i] + o;
    const bool with_rom = has_rom(o);
    const Derived d = derive_instruction(aux_fns, rom_pc, with_rom, alias_pc, 3U);
    Row row{o, d.ok, d.reason, false, with_rom};
    if (d.ok) {
      const std::string name = "genesis_sr_" + hex8(o);
      const std::string shared = shared_function(name, d);
      // exact equivalence: instantiating the shared pieces at every real address reproduces that address's own lowering
      bool equal = !shared.empty();
      const auto check = [&](std::uint32_t pc, bool alias) {
        std::string body = fns.at(pc).body;
        std::string guard, prov;
        if (alias) take_line(body, kGuardPrefix, guard);
        take_line(body, "  const GenesisInstructionProvenance *const genesis_aot_source", prov);
        // rebuild from the derived pieces
        std::string body_derived = instantiate(d.body, pc, "genesis_pc0");
        std::string prov_derived = instantiate(d.provenance, pc, "genesis_pc0");
        std::string guard_derived = alias ? instantiate(d.guard, pc, "genesis_pc0") : std::string();
        const bool prov_ok = prov.empty() || prov == prov_derived;
        std::string stripped = body_derived;
        std::string dropped;
        take_line(stripped, "  const GenesisInstructionProvenance *const genesis_aot_source", dropped);
        if (std::getenv("SR_DEBUG") != nullptr && !(prov_ok && stripped == body && (!alias || canon_guard(guard) == canon_guard(guard_derived))))
          std::fprintf(stderr, "pc=%s prov_ok=%d body_eq=%d guard_eq=%d\n--stripped--\n%s\n--body--\n%s\n--guard--\n%s\n--gd--\n%s\n", hex8(pc).c_str(), prov_ok, stripped == body, canon_guard(guard) == canon_guard(guard_derived), stripped.c_str(), body.c_str(), guard.c_str(), guard_derived.c_str());
        return prov_ok && stripped == body && (!alias || canon_guard(guard) == canon_guard(guard_derived));
      };
      if (with_rom) equal = equal && check(rom_pc, false);
      for (unsigned i = 0; i < k; ++i) equal = equal && check(kAliasBases[i] + o, true);
      row.equivalent = equal;
      if (equal) {
        definitions += shared;
        if (with_rom) {
          renames["genesis_aot_" + hex8(rom_pc)] = name;
          cuts.emplace_back(fns.at(rom_pc).begin, fns.at(rom_pc).end);
        }
        for (unsigned i = 0; i < k; ++i) renames["genesis_aot_" + hex8(kAliasBases[i] + o)] = name;
        for (unsigned i = 0; i < k; ++i) cuts.emplace_back(fns.at(kAliasBases[i] + o).begin, fns.at(kAliasBases[i] + o).end);
        ++plan.shared;
      } else {
        plan.unverified.push_back(hex8(o));
      }
    } else {
      ++plan.fallbacks;
      plan.reasons.push_back(hex8(o) + ":" + d.reason);
    }
    ++plan.instructions;
    rows.push_back(row);
  }
  std::sort(cuts.rbegin(), cuts.rend());
  std::size_t insert_at = text.size();
  for (const auto &[b, e] : cuts) insert_at = std::min(insert_at, b);
  for (const auto &[b, e] : cuts) text.erase(b, e - b);
  if (!cuts.empty()) text.insert(insert_at, definitions);
  for (const auto &[from, to] : renames)
    for (std::size_t at = 0; (at = text.find(from, at)) != std::string::npos; at += to.size()) text.replace(at, from.size(), to);
  if (mode == "--shared") { std::cout << text; return 0; }
  if (mode == "--report") {
    std::size_t ref_functions = 0, shared_functions = 0;
    for (std::size_t at = 0; (at = reference.find("static GenesisControlTransfer genesis_aot_", at)) != std::string::npos; ++at) ++ref_functions;
    for (std::size_t at = 0; (at = text.find("static GenesisControlTransfer genesis_aot_", at)) != std::string::npos; ++at) ++shared_functions;
    for (std::size_t at = 0; (at = text.find("static GenesisControlTransfer genesis_sr_", at)) != std::string::npos; ++at) ++shared_functions;
    unsigned with_rom_count = 0;
    for (const auto &row : rows) with_rom_count += row.with_rom && row.equivalent ? 1U : 0U;
    std::cout << "{\"aliases\":" << k << ",\"routine_instructions\":" << offsets.size() << ",\"shared_with_rom\":" << with_rom_count << ",\"derived\":" << plan.shared << ",\"fallbacks\":" << plan.fallbacks
              << ",\"unverified\":" << plan.unverified.size() << ",\"functions_reference\":" << ref_functions << ",\"functions_shared\":" << shared_functions
              << ",\"c_bytes_reference\":" << reference.size() << ",\"c_bytes_shared\":" << text.size() << ",\"reasons\":[";
    for (std::size_t i = 0; i < plan.reasons.size(); ++i) std::cout << (i ? "," : "") << "\"" << plan.reasons[i] << "\"";
    std::cout << "],\"rows\":[";
    for (std::size_t i = 0; i < rows.size(); ++i)
      std::cout << (i ? "," : "") << "{\"offset\":" << rows[i].offset << ",\"with_rom\":" << (rows[i].with_rom ? "true" : "false") << ",\"derived\":" << (rows[i].ok ? "true" : "false") << ",\"equivalent\":" << (rows[i].equivalent ? "true" : "false") << "}";
    std::cout << "]}\n";
    return 0;
  }
  return 2;
}
