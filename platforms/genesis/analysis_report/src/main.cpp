// SEG-030-T002 (ADR 0079, report-only): `segarecomp-genesis-analysis-report`, the multi-title report driver of the M68K
// instantiation of the abstract-analysis core. Never linked into, or invoked by, the `segarecomp` CLI or any production route.
//
// stdout: sanitized aggregate JSON only (counts and generic classes). --private-output: exact PCs (challenger private v1
// superset); for a commercial input the caller must keep it in an ignored location and never persist it. Wall time and peak RSS
// go only to --metrics-output, so the other outputs are byte-identical across runs.

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#else
#include <sys/resource.h>
#endif

#include "segarecomp/cpu/m68k/analysis/abstract_memory.hpp"
#include "segarecomp/genesis_analysis_report/hybrid_plan.hpp"
#include "segarecomp/genesis_analysis_report/report.hpp"
#include "segarecomp/rom.hpp"
#include "segarecomp/sha256.hpp"

namespace {

void usage(std::ostream &out) {
  out << "usage: segarecomp-genesis-analysis-report --rom <image> --rom-sha256 <sha256> (--reset-entry | --entry <address-hex8> "
         "--mapping-base <address-hex8>) [--immutable-copy-alias <execution-hex8>:<source-hex8>:<length-hex8>]... --private-output "
         "<path> [--universe] [--domains baseline|<address,memory,contexts,frames>|all] [--compare-challenger] [--metrics-output "
         "<path>] [--max-iterations <n>] [--max-points <n>] [--assume-no-z80-ram-writes (diagnostic premise ablation; needs memory)] "
         "[--hybrid-plan <plan-path> (SEG-031: plan the hybrid admission; forces --domains all)] "
         "[--trace-points <path> (SEG-034: private per-point state dump of the final solve)] "
         "[--diagnostic-transparent-handlers (SEG-034: uncredited ablation: unanalysed interrupt handlers are transparent; writes no plan)] "
         "[--inspect-cells <offset-hex:width>[,<offset-hex:width>...] (SEG-038: uncredited report-only mutable-RAM cell domain "
         "inspection; needs --trace-points and --domains memory|contexts|frames|all; never production authority)] "
         "[--z80-image <path> (SEG-041-T003: report-only, caller-supplied Z80 RAM image bytes from $0000, at most 8 KiB; forces "
         "--domains memory or wider; feeds the existing config.z80_images input the production driver already consumes when "
         "non-nullopt, exactly as a same-process derived image would -- never a build/runtime dependency, diagnostic only)] "
         "[--external-m68k-facts <path> (SEG-041-T008: report-only, caller-supplied externally-proven dynamic-control-site facts, "
         "`segarecomp.m68k_external_facts.v1`; requires --hybrid-plan; every fact is structurally re-verified against this "
         "run's own decoder/legality authority before being trusted -- a malformed, mismatched-ROM, or unverifiable file is "
         "rejected outright, never partially trusted; never a build/runtime dependency, diagnostic only)] "
         "[--inspect-all-cells (SEG-038: uncredited report-only dump of every distinct cell the solve ever tracked, address-"
         "agnostic; same requirements as --inspect-cells)]\n";
}

// SEG-038-T001 (report-only, uncredited diagnostic): one requested mutable-RAM cell (physical offset in the work-RAM mirror,
// width 1/2/4 bytes) and the aggregate domain the final solve actually reaches for it, across every point reached by the solver.
// This never supplies a production fact; it only answers "does this cell have a small, apparently-complete finite domain once the
// solve is examined directly", independent of any title-specific address knowledge baked into source (the caller supplies the
// offset at the command line or from an ignored local file; this tool never stores or interprets what the offset means).
struct CellInspection {
  std::uint32_t offset{};
  std::uint32_t width{};
};

std::optional<std::vector<CellInspection>> parse_cell_inspections(std::string_view text) {
  std::vector<CellInspection> cells;
  while (!text.empty()) {
    const auto comma = text.find(',');
    const auto entry = text.substr(0, comma);
    const auto colon = entry.find(':');
    if (colon == std::string_view::npos) return std::nullopt;
    const auto offset_text = entry.substr(0, colon);
    const auto width_text = entry.substr(colon + 1U);
    if (offset_text.empty() || offset_text.size() > 4U) return std::nullopt;
    std::uint64_t offset{};
    const auto offset_result = std::from_chars(offset_text.data(), offset_text.data() + offset_text.size(), offset, 16);
    if (offset_result.ec != std::errc{} || offset_result.ptr != offset_text.data() + offset_text.size()) return std::nullopt;
    std::uint64_t width{};
    const auto width_result = std::from_chars(width_text.data(), width_text.data() + width_text.size(), width, 10);
    if (width_result.ec != std::errc{} || width_result.ptr != width_text.data() + width_text.size() ||
        (width != 1U && width != 2U && width != 4U)) {
      return std::nullopt;
    }
    cells.push_back({static_cast<std::uint32_t>(offset), static_cast<std::uint32_t>(width)});
    if (comma == std::string_view::npos) break;
    text.remove_prefix(comma + 1U);
    if (text.empty()) return std::nullopt;
  }
  if (cells.empty()) return std::nullopt;
  return cells;
}

std::optional<std::uint64_t> parse_hex(std::string_view text, std::size_t width) {
  if (text.size() != width) return std::nullopt;
  std::uint64_t value{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return std::nullopt;
  return value;
}

std::optional<std::size_t> parse_count(std::string_view text) {
  if (text.empty() || text.size() > 12U) return std::nullopt;
  std::size_t value{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 10);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value == 0U) return std::nullopt;
  return value;
}

std::optional<segarecomp::GenesisAnalysisDomains> parse_domains(std::string_view text) {
  segarecomp::GenesisAnalysisDomains domains{};
  if (text == "baseline") return domains;
  if (text == "all") return segarecomp::GenesisAnalysisDomains{true, true, true, true};
  while (!text.empty()) {
    const auto comma = text.find(',');
    const auto name = text.substr(0, comma);
    bool *flag = name == "address" ? &domains.address : name == "memory" ? &domains.memory : name == "contexts" ? &domains.contexts
                 : name == "frames"                                                                              ? &domains.frames
                                                                                                                  : nullptr;
    if (flag == nullptr || *flag) return std::nullopt;
    *flag = true;
    if (comma == std::string_view::npos) break;
    text.remove_prefix(comma + 1U);
    if (text.empty()) return std::nullopt;
  }
  if (domains.baseline()) return std::nullopt;
  return domains;
}

// Untrusted input: read at most `image_size_limit` + 1 bytes (a device or an oversized file is rejected before hashing, never
// read unbounded).
std::optional<std::vector<std::uint8_t>> read_bounded_image(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::nullopt;
  std::vector<std::uint8_t> bytes(segarecomp::image_size_limit + 1U);
  input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  const auto count = static_cast<std::size_t>(input.gcount());
  if (count > segarecomp::image_size_limit || input.bad()) return std::nullopt;
  bytes.resize(count);
  return bytes;
}

std::uint64_t peak_rss_bytes() {
#if defined(_WIN32)
  return 0U;
#else
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return 0U;
#if defined(__APPLE__)
  return static_cast<std::uint64_t>(usage.ru_maxrss);  // bytes
#else
  return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024U;  // KiB
#endif
#endif
}

int run(int argc, char **argv) {
  std::optional<std::string> rom, digest, private_output, metrics_output, hybrid_plan, trace_points, inspect_cells_spec, z80_image,
      external_m68k_facts_path;
  bool diagnostic_transparent = false, inspect_all_cells = false;
  std::optional<std::uint32_t> entry_address, mapping_base;
  bool reset_entry = false, universe = false, compare = false, domains_given = false, iterations_given = false,
       points_given = false, assume_no_z80 = false;
  segarecomp::GenesisAnalysisReportConfig config{};
  std::vector<std::array<std::uint32_t, 3>> aliases;
  for (int index = 1; index < argc;) {
    const std::string_view option = argv[index];
    const bool has_value = index + 1 < argc;
    const std::string_view value = has_value ? std::string_view(argv[index + 1]) : std::string_view{};
    if (option == "--rom" && has_value && !rom) rom = std::string(value);
    else if (option == "--rom-sha256" && has_value && !digest) digest = std::string(value);
    else if (option == "--private-output" && has_value && !private_output) private_output = std::string(value);
    else if (option == "--metrics-output" && has_value && !metrics_output) metrics_output = std::string(value);
    else if (option == "--hybrid-plan" && has_value && !hybrid_plan) hybrid_plan = std::string(value);
    else if (option == "--trace-points" && has_value && !trace_points) trace_points = std::string(value);
    else if (option == "--inspect-cells" && has_value && !inspect_cells_spec) inspect_cells_spec = std::string(value);
    else if (option == "--z80-image" && has_value && !z80_image) z80_image = std::string(value);
    else if (option == "--external-m68k-facts" && has_value && !external_m68k_facts_path) external_m68k_facts_path = std::string(value);
    else if (option == "--inspect-all-cells" && !inspect_all_cells) { inspect_all_cells = true; ++index; continue; }
    else if (option == "--diagnostic-transparent-handlers" && !diagnostic_transparent) { diagnostic_transparent = true; ++index; continue; }
    else if (option == "--reset-entry" && !reset_entry) { reset_entry = true; ++index; continue; }
    else if (option == "--universe" && !universe) { universe = true; ++index; continue; }
    else if (option == "--compare-challenger" && !compare) { compare = true; ++index; continue; }
    else if (option == "--domains" && has_value && !domains_given) {
      const auto domains = parse_domains(value);
      if (!domains) { usage(std::cerr); return 2; }
      config.domains = *domains;
      domains_given = true;
    } else if (option == "--assume-no-z80-ram-writes" && !assume_no_z80) {
      assume_no_z80 = true;
      ++index;
      continue;
    } else if ((option == "--max-iterations" || option == "--max-points") && has_value) {
      auto &given = option == "--max-iterations" ? iterations_given : points_given;
      const auto count = parse_count(value);
      if (!count || given) { usage(std::cerr); return 2; }
      given = true;
      (option == "--max-iterations" ? config.bounds.max_iterations : config.bounds.max_points) = *count;
    } else if ((option == "--entry" || option == "--mapping-base") && has_value) {
      const auto parsed = parse_hex(value, 8);
      auto &destination = option == "--entry" ? entry_address : mapping_base;
      if (!parsed || destination || (*parsed & UINT64_C(0xFF000000)) != 0U || (option == "--entry" && (*parsed & 1U) != 0U)) {
        std::cerr << "segarecomp-genesis-analysis-report: invalid input\n";
        return 2;
      }
      destination = static_cast<std::uint32_t>(*parsed);
    } else if (option == "--immutable-copy-alias" && has_value) {
      const auto first = value.find(':');
      const auto second = first == std::string_view::npos ? first : value.find(':', first + 1U);
      if (second == std::string_view::npos) { usage(std::cerr); return 2; }
      const auto execution = parse_hex(value.substr(0, first), 8);
      const auto source = parse_hex(value.substr(first + 1U, second - first - 1U), 8);
      const auto length = parse_hex(value.substr(second + 1U), 8);
      if (!execution || !source || !length) { usage(std::cerr); return 2; }
      aliases.push_back({static_cast<std::uint32_t>(*execution), static_cast<std::uint32_t>(*source), static_cast<std::uint32_t>(*length)});
    } else {
      usage(std::cerr);
      return 2;
    }
    index += 2;
  }
  if (!rom || !digest || !private_output || (reset_entry == (entry_address.has_value() || mapping_base.has_value())) ||
      (!reset_entry && (!entry_address || !mapping_base))) {
    usage(std::cerr);
    return 2;
  }
  // ADR 0079 decision 5: the frames domain needs the contexts domain (handler partitions are contexts), which needs the memory
  // domain (callee summaries carry memory effects), which needs the address domain (its cells are addressed through points-to values).
  if (config.domains.frames) config.domains.contexts = true;
  if (config.domains.contexts) config.domains.memory = true;
  if (config.domains.memory) config.domains.address = true;
  if (assume_no_z80 && !config.domains.memory) { usage(std::cerr); return 2; }
  if (z80_image && (!config.domains.memory || assume_no_z80)) { usage(std::cerr); return 2; }
  if (external_m68k_facts_path && !hybrid_plan) { usage(std::cerr); return 2; }
  if (diagnostic_transparent && !hybrid_plan && !config.domains.frames) { usage(std::cerr); return 2; }
  std::optional<std::vector<CellInspection>> inspect_cells;
  if (inspect_cells_spec) {
    inspect_cells = parse_cell_inspections(*inspect_cells_spec);
    if (!inspect_cells || !trace_points || !config.domains.memory || hybrid_plan) { usage(std::cerr); return 2; }
  }
  if (inspect_all_cells && (!trace_points || !config.domains.memory || hybrid_plan)) { usage(std::cerr); return 2; }
  config.diagnostic_transparent_handlers = diagnostic_transparent;
  config.assume_no_z80_ram_writes = assume_no_z80;
  config.reset_entry = reset_entry;
  const auto started = std::chrono::steady_clock::now();
  const auto read = read_bounded_image(*rom);
  if (!read) {
    std::cerr << "segarecomp-genesis-analysis-report: cannot read the image (missing, unreadable or larger than the image limit)\n";
    return 2;
  }
  const auto &bytes = *read;
  std::string expected = *digest;
  std::transform(expected.begin(), expected.end(), expected.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (segarecomp::sha256_hex(bytes) != expected) {
    std::cerr << "segarecomp-genesis-analysis-report: image digest mismatch\n";
    return 2;
  }
  if (z80_image) {
    const auto z80_bytes = read_bounded_image(*z80_image);
    if (!z80_bytes || z80_bytes->size() > 0x2000U) {
      std::cerr << "segarecomp-genesis-analysis-report: invalid --z80-image (missing, unreadable or larger than 8 KiB)\n";
      return 2;
    }
    config.z80_images = std::vector<segarecomp::GenesisZ80Image>{
        segarecomp::GenesisZ80Image{*z80_bytes, "external_concrete_execution"}};
  }
  std::optional<segarecomp::FrontendProgram> program;
  if (reset_entry) {
    const auto reset = segarecomp::analyze_genesis_reset_image(bytes);
    if (reset.outcome != segarecomp::ResetOutcome::accepted) {
      std::cerr << segarecomp::format_genesis_reset_image_report(reset) << '\n';
      return 1;
    }
    program = segarecomp::make_genesis_reset_bridge_startup_program(bytes, reset, std::nullopt);
  } else {
    program = segarecomp::make_genesis_bridge_startup_program(bytes, *mapping_base, *entry_address, std::nullopt);
  }
  if (!program) { std::cerr << "segarecomp-genesis-analysis-report: bridge mapping overflows\n"; return 2; }
  // ADR 0049 aliases are machine mapping facts, recorded through the same fail-closed owner the challenger CLI uses.
  if (!aliases.empty() && !segarecomp::apply_genesis_immutable_rom_aot(*program)) {
    std::cerr << "segarecomp-genesis-analysis-report: invalid immutable-ROM AOT mapping source\n";
    return 2;
  }
  for (const auto &alias : aliases)
    if (!segarecomp::apply_genesis_immutable_copy_alias(*program, alias[0], alias[1], alias[2])) {
      std::cerr << "segarecomp-genesis-analysis-report: invalid immutable-copy alias\n";
      return 2;
    }

  if (hybrid_plan) {
    // SEG-031 (ADR 0080): the hybrid planner (forces `--domains all`; the challenger comparison and premise ablation do not apply).
    if (compare || assume_no_z80 || (domains_given && !(config.domains.address && config.domains.memory && config.domains.contexts &&
                                                        config.domains.frames))) {
      usage(std::cerr);
      return 2;
    }
    segarecomp::GenesisHybridPlanConfig plan_config{};
    plan_config.analysis = config;
    plan_config.diagnostic_transparent_handlers = diagnostic_transparent;
    if (external_m68k_facts_path) {
      const auto raw = read_bounded_image(*external_m68k_facts_path);
      if (!raw) {
        std::cerr << "segarecomp-genesis-analysis-report: cannot read --external-m68k-facts\n";
        return 2;
      }
      const std::string text(raw->begin(), raw->end());
      const auto parsed = segarecomp::parse_genesis_external_m68k_facts(text, expected);
      if (!parsed) {
        std::cerr << "segarecomp-genesis-analysis-report: invalid --external-m68k-facts (malformed, oversized, or ROM-hash "
                      "mismatch; rejected outright, never partially trusted)\n";
        return 2;
      }
      plan_config.external_m68k_facts = *parsed;
    }
    const auto plan = segarecomp::plan_genesis_hybrid_admission(*program, plan_config);
    {
      std::ofstream sink{*private_output, std::ios::binary};
      sink << segarecomp::format_genesis_hybrid_plan_private(plan) << '\n';
      if (!sink) { std::cerr << "segarecomp-genesis-analysis-report: cannot write private output\n"; return 2; }
    }
    if (!diagnostic_transparent) {  // a diagnostic ablation never writes a production plan
      std::ofstream sink{*hybrid_plan, std::ios::binary};
      sink << segarecomp::format_genesis_hybrid_admission_plan(segarecomp::genesis_hybrid_admission_plan(plan, *program, expected));
      if (!sink) { std::cerr << "segarecomp-genesis-analysis-report: cannot write hybrid plan\n"; return 2; }
    }
    if (metrics_output) {
      const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
      std::ofstream metrics{*metrics_output, std::ios::binary};
      metrics << "{\"schema\":\"segarecomp.m68k_hybrid_plan.metrics.v1\",\"wall_seconds\":" << std::fixed << std::setprecision(3)
              << seconds << ",\"peak_rss_bytes\":" << peak_rss_bytes() << "}\n";
      if (!metrics) { std::cerr << "segarecomp-genesis-analysis-report: cannot write metrics output\n"; return 2; }
    }
    std::cout << segarecomp::format_genesis_hybrid_plan_aggregate(plan) << '\n';
    return plan.outcome == segarecomp::GenesisHybridOutcome::broad_images_invalid ? 1 : 0;
  }
  auto report = segarecomp::run_genesis_analysis_report(*program, config);
  if (trace_points && report.images_valid) {
    // SEG-034 private diagnostic: the abstract state at every point of the final solve (exact PCs; never persisted).
    std::ofstream sink{*trace_points, std::ios::binary};
    for (const auto &[point, state] : report.analysis.solution.in_states) {
      sink << std::hex << std::setw(2) << std::setfill('0') << segarecomp::m68k_point_tag(point) << ' ' << std::setw(6)
           << segarecomp::m68k_point_pc(point) << std::dec << " ctx=" << segarecomp::m68k_context_site(segarecomp::m68k_point_context(point))
           << " delta=" << state.stack_delta.describe() << " status=" << state.status.describe() << " A7=" << state.address[7].describe()
           << " A6=" << state.address[6].describe() << " A5=" << state.address[5].describe() << " A4=" << state.address[4].describe() << " A3=" << state.address[3].describe() << " A2=" << state.address[2].describe() << " A1=" << state.address[1].describe() << " A0=" << state.address[0].describe()
           << " D0w=" << state.values.values[segarecomp::m68k_analysis_slot(0, 16)].describe() << " rn=" << int(state.resumption_unknown)
           << [&] {  // SEG-035: every data-register slot and its CPU-owned width-derived flag (private output only)
                std::string slots;
                for (unsigned reg = 0; reg < 8U; ++reg)
                  for (const unsigned width : {16U, 32U}) {
                    const auto slot = segarecomp::m68k_analysis_slot(reg, width);
                    slots += " D" + std::to_string(reg) + (width == 16U ? "w=" : "l=") + state.values.values[slot].describe() +
                             (state.width_derived[slot] ? "[W]" : "");
                  }
                return slots;
              }()
           << '\n';
    }
    if (inspect_cells) {
      // SEG-038-T001 (report-only, uncredited): for each requested mutable-RAM cell, join its value across every point the
      // solver actually reached and report whether that aggregate domain is a small finite set, Unknown, or never observed.
      // This never emits a plan and never feeds `apply_resumptions` or any production consumer; it is private-output only.
      // Reuses the already-open `sink` above (never a second independent stream on the same path: a second handle's
      // buffered flush at its own destructor can clobber bytes the first handle already wrote past that point).
      for (const auto &cell : *inspect_cells) {
        const segarecomp::M68kCell key{segarecomp::M68kRegionKind::mutable_ram, 0U, cell.offset, cell.width};
        std::optional<segarecomp::M68kCellValue> joined;
        bool collapsed = false;  // sticky: the aggregate join already exceeded the finite-set bound (treat as Unknown)
        std::size_t present{}, total{};
        for (const auto &[point, state] : report.analysis.solution.in_states) {
          ++total;
          const auto found = state.memory.cells.find(key);
          if (found == state.memory.cells.end()) continue;
          ++present;
          if (collapsed) continue;
          if (!joined) { joined = found->second; continue; }
          const auto next = segarecomp::m68k_cell_join(*joined, found->second, cell.width);
          if (!next) { collapsed = true; joined.reset(); continue; }
          joined = next;
        }
        sink << "CELL offset=" << std::hex << std::setw(4) << std::setfill('0') << cell.offset << std::dec << " width=" << cell.width
             << " present=" << present << "/" << total << " domain="
             << (collapsed ? std::string("unknown(join_collapsed)") : joined ? joined->data.describe() : std::string("never_present"))
             << " credited=false\n";
      }
    }
    if (inspect_all_cells) {
      // SEG-038-T001 (report-only, uncredited, address-agnostic): every distinct mutable-RAM cell the solve ever tracked at
      // any reached point, joined across every point it appears at. No offset is supplied by the caller or baked into source;
      // this answers "what, if anything, did the solve ever manage to track" without any title-specific hint.
      std::map<segarecomp::M68kCell, std::optional<segarecomp::M68kCellValue>> joined_by_cell;
      std::map<segarecomp::M68kCell, bool> collapsed_by_cell;
      std::map<segarecomp::M68kCell, std::size_t> present_by_cell;
      std::size_t total = 0U;
      for (const auto &[point, state] : report.analysis.solution.in_states) {
        ++total;
        for (const auto &[cell, value] : state.memory.cells) {
          ++present_by_cell[cell];
          auto &collapsed = collapsed_by_cell[cell];
          if (collapsed) continue;
          auto &joined = joined_by_cell[cell];
          if (!joined) { joined = value; continue; }
          const auto next = segarecomp::m68k_cell_join(*joined, value, cell.width);
          if (!next) { collapsed = true; joined.reset(); continue; }
          joined = next;
        }
      }
      for (const auto &[cell, present] : present_by_cell) {
        const auto joined = joined_by_cell.at(cell);
        sink << "ALLCELL offset=" << std::hex << std::setw(4) << std::setfill('0') << cell.offset << std::dec << " width=" << cell.width
             << " present=" << present << "/" << total << " domain="
             << (collapsed_by_cell.at(cell) ? std::string("unknown(join_collapsed)")
                                            : joined ? joined->data.describe() : std::string("never_present"))
             << " credited=false\n";
      }
    }
  }
  if (!report.images_valid) {
    std::cerr << "segarecomp-genesis-analysis-report: executable-image set rejected\n";
    return 1;
  }
  if (universe) {
    report.universe = segarecomp::genesis_analysis_universe(*program);
    if (!report.universe) { std::cerr << "segarecomp-genesis-analysis-report: broad analysis rejected\n"; return 1; }
  }
  std::string aggregate = segarecomp::format_genesis_analysis_report_aggregate(report, config);
  if (diagnostic_transparent)
    aggregate.insert(aggregate.size() - 1U, ",\"diagnostic_ablation\":\"transparent_handlers\",\"credited\":false");
  std::string extra;
  if (compare) {
    segarecomp::GenesisReachabilityChallengerConfig challenger_config{};
    challenger_config.exception_model = segarecomp::GenesisReachabilityExceptionModel::strict;
    challenger_config.pc_index_recovery = true;
    const auto challenger = segarecomp::run_genesis_reachability_challenger(*program, challenger_config);
    const auto comparison = segarecomp::compare_genesis_analysis_with_challenger(report, challenger);
    aggregate.insert(aggregate.size() - 1U,
                     ",\"challenger_comparison\":" + segarecomp::format_genesis_analysis_comparison_aggregate(comparison));
    extra = ",\"challenger_comparison\":" + segarecomp::format_genesis_analysis_comparison_private(comparison);
  }
  {
    std::ofstream sink{*private_output, std::ios::binary};
    sink << segarecomp::format_genesis_analysis_report_private(report, aggregate, extra);
    if (!sink) { std::cerr << "segarecomp-genesis-analysis-report: cannot write private output\n"; return 2; }
  }
  if (metrics_output) {
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::ofstream metrics{*metrics_output, std::ios::binary};
    metrics << "{\"schema\":\"segarecomp.m68k_core_report.metrics.v1\",\"wall_seconds\":" << std::fixed << std::setprecision(3)
            << seconds << ",\"peak_rss_bytes\":" << peak_rss_bytes() << "}\n";
    if (!metrics) { std::cerr << "segarecomp-genesis-analysis-report: cannot write metrics output\n"; return 2; }
  }
  std::cout << aggregate << '\n';
  // A solve that exhausted a bound has no discovered set (every query Unknown): the outputs say so (`solver.complete` false) and
  // the run fails, so no caller mistakes it for an empty result.
  return report.analysis.complete ? 0 : 3;
}

}  // namespace

int main(int argc, char **argv) {
#if defined(_WIN32)
  (void)_setmode(_fileno(stdout), _O_BINARY);
  (void)_setmode(_fileno(stderr), _O_BINARY);
#endif
  try {
    return run(argc, argv);
  } catch (const std::exception &error) {
    std::cerr << "segarecomp-genesis-analysis-report: " << error.what() << '\n';
    return 1;
  }
}
