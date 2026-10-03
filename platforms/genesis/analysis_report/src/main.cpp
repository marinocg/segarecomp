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

#include "segarecomp/genesis_analysis_report/report.hpp"
#include "segarecomp/rom.hpp"
#include "segarecomp/sha256.hpp"

namespace {

void usage(std::ostream &out) {
  out << "usage: segarecomp-genesis-analysis-report --rom <image> --rom-sha256 <sha256> (--reset-entry | --entry <address-hex8> "
         "--mapping-base <address-hex8>) [--immutable-copy-alias <execution-hex8>:<source-hex8>:<length-hex8>]... --private-output "
         "<path> [--universe] [--domains baseline|<address,memory,contexts,frames>|all] [--compare-challenger] [--metrics-output "
         "<path>] [--max-iterations <n>] [--max-points <n>] [--assume-no-z80-ram-writes (diagnostic premise ablation; needs memory)]\n";
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
  std::optional<std::string> rom, digest, private_output, metrics_output;
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
  // ADR 0079 decision 5: the contexts domain needs the memory domain (callee summaries carry memory effects), which needs the
  // address domain (its cells are addressed through points-to values).
  if (config.domains.contexts) config.domains.memory = true;
  if (config.domains.memory) config.domains.address = true;
  if (assume_no_z80 && !config.domains.memory) { usage(std::cerr); return 2; }
  config.assume_no_z80_ram_writes = assume_no_z80;
  if (config.domains.frames) {
    // ADR 0079: address (T003), memory (T004) and contexts (T005) are delivered; frames by T006 (fail closed).
    std::cerr << "segarecomp-genesis-analysis-report: staged domain not implemented\n";
    return 2;
  }
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

  auto report = segarecomp::run_genesis_analysis_report(*program, config);
  if (!report.images_valid) {
    std::cerr << "segarecomp-genesis-analysis-report: executable-image set rejected\n";
    return 1;
  }
  if (universe) {
    report.universe = segarecomp::genesis_analysis_universe(*program);
    if (!report.universe) { std::cerr << "segarecomp-genesis-analysis-report: broad analysis rejected\n"; return 1; }
  }
  std::string aggregate = segarecomp::format_genesis_analysis_report_aggregate(report, config);
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
