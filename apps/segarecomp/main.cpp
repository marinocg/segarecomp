#include "segarecomp/c_emitter.hpp"
#include "segarecomp/direct_flow.hpp"
#include "segarecomp/moveq.hpp"
#include "segarecomp/codegen/c11/genesis_frontend.hpp"
#include "segarecomp/codegen/c11/provenance_diagnostics.hpp"
#include "segarecomp/machine/genesis/frontend.hpp"
#include "segarecomp/machine/genesis/hybrid_admission.hpp"
#include "segarecomp/machine/genesis/ml_region_producer.hpp"
#include "segarecomp/sha256.hpp"
#include "segarecomp/machine/genesis/reachability_challenger.hpp"
#include "build_command.hpp"
#include "segarecomp/rom.hpp"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#endif

#include <algorithm>
#include "segarecomp/codegen/c11/translation_units.hpp"
#include <array>
#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
void print_usage(std::ostream &output) {
  output << "usage:\n" << segarecomp_build_usage << "  segarecomp inspect <image>\n  segarecomp analyze <image>\n  segarecomp emit-c <image>\n"
             "  segarecomp emit-moveq-c <image> <pc> <offset> <sr> <d0> <d1> <d2> <d3> <d4> <d5> <d6> <d7>\n"
               "  segarecomp emit-direct-flow-c <image> <pc> <offset> <sr> <budget> <d0> <d1> <d2> <d3> <d4> <d5> <d6> <d7>\n"
               "  segarecomp m68k-frontend <image> <source-id> <entry> <claim-name> <target-begin> <target-end> <image-begin> <image-end> [... ]\n"
               "  segarecomp emit-m68k-frontend-c <image> <source-id> <analysis-entry> <execution-entry> <sr> <budget> <d0> <d1> <d2> <d3> <d4> <d5> <d6> <d7> <claim-name> <target-begin> <target-end> <image-begin> <image-end> [... ]\n"
                "  segarecomp genesis-rom-startup <image>\n  segarecomp emit-genesis-rom-startup-c <image>\n"
                "  segarecomp genesis-general-startup <image>\n"
                  "  segarecomp emit-general-startup-bridge-c --rom <image> (--reset-entry [--analysis-seed <address-hex8>]... | --entry <address-hex8> --mapping-base <address-hex8>) --rom-sha256 <sha256> [--external-hints <path>] [--immutable-aot-address-report <path>] [--direct-control-address-report <path>] [--window-feature-report <path> --window-feature-bytes <256|512>] [--ml-region-proposal-output <path>] [--immutable-aot-region-proposal <path> --region-admission-plan-output <path>] [--legacy-aot-entries] [--immutable-rom-aot [--immutable-copy-alias <execution-hex8>:<source-hex8>:<length-hex8>]... [--immutable-rom-aot-admission <plan> | --immutable-rom-aot-ml-admission]] [--provenance-diagnostics] [--generated-c-output <path>] [--generated-c-shard-dir <dir>]\n"
                 "  segarecomp genesis-reachability-challenger --rom <image> (--reset-entry | --entry <address-hex8> --mapping-base <address-hex8>) --rom-sha256 <sha256> --private-output <path> [--immutable-copy-alias <execution-hex8>:<source-hex8>:<length-hex8>]... [--exception-model strict|normal-resumption] [--pea-continuations] [--pc-index-recovery [--pc-index-width-domains]] [--universe] [--classify-pcs <path> --classify-output <path>]\n"
                 "  segarecomp emit-genesis-pc-relative-offset-table-proposals --rom <image> --reset-entry --rom-sha256 <sha256> [--external-hints <path>]\n"
               "  segarecomp probe-genesis-startup-decode <primary-hex4> <extension-hex8-or-dash>\n"
               "  segarecomp probe-genesis-startup-mapping <address-hex8> <width-decimal> <image-length-hex16>\n";
}
[[nodiscard]] std::optional<std::uint64_t> parse_hex(std::string_view text, std::size_t width) {
  if (text.size() != width) return std::nullopt;
  std::uint64_t value{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return std::nullopt;
  return value;
}
} // namespace

int run_cli(int argc, char **argv) {
#if defined(_WIN32)
  // SEG-018-T006: generated C and reports are deterministic byte streams; never
  // let the Windows CRT translate "\n" to "\r\n" on stdout/stderr.
  (void)_setmode(_fileno(stdout), _O_BINARY);
  (void)_setmode(_fileno(stderr), _O_BINARY);
#endif
  if (argc < 2) { print_usage(std::cerr); return 2; }
  try {
    const std::string_view command = argv[1];
    if (command == "emit-general-startup-bridge-c") {
      if (argc < 7 || std::string_view(argv[2]) != "--rom") {
        print_usage(std::cerr); return 2;
      }
      std::optional<std::uint32_t> entry_address;
      std::optional<std::uint32_t> mapping_base;
      bool reset_entry = false;
      std::optional<std::pair<std::uint32_t, std::uint32_t>> synthetic_completion;
      std::optional<std::string_view> digest;
      // ADR-0013 Decision §7 Phase B: an ordered, repeatable, build-time
      // seed-transport CLI surface. Each value must be an even 24-bit
      // address, exactly like --entry; the driver is solely responsible for
      // ensuring every supplied value is either statically proven or
      // runtime_confirmed (ADR-0011 §5) before it ever reaches this CLI.
      std::vector<std::uint32_t> analysis_seeds;
      // SEG-007-T164 / ADR-0023: the required, explicit, per-invocation
      // opt-in for the external logical-table-descriptor hints interchange
      // file. Absent this flag, `external_hints_path` stays unset and
      // `program->external_logical_table_descriptor_hints` stays empty --
      // ordinary raw-ROM recompilation is entirely unaffected.
      std::optional<std::string_view> external_hints_path;
      // Explicit non-default gate for complete mapping-derived immutable-ROM
      // AOT enumeration. No caller address/range enters this source.
      bool immutable_rom_aot = false;
      // SEG-021-T041 / ADR 0049: generated-data immutable-copy alias proposals (execution:source:length).
      std::vector<std::array<std::uint32_t, 3>> immutable_copy_aliases;
      // SEG-020-T002: opt-in provenance diagnostic table appended to the generated C.
      bool provenance_diagnostics = false;
      // SEG-022-T001: opt-in, measurement-only sink for the admitted immutable-ROM AOT
      // address set (sorted, one hex address per line). Ephemeral: the measurement tool
      // hashes it and never stores the addresses. It reads the existing analysis result
      // and cannot alter generation.
      std::optional<std::string_view> immutable_aot_address_report;
      // SEG-031 (ADR 0080): explicit hybrid admission candidate. A plan produced by the report-only planner names the admitted subset
      // of the broad immutable-ROM AOT identities; it is validated fail-closed (digest, alias set, structural closure) before the
      // identities are filtered. Absent: broad admission, byte-identical to before SEG-031.
      std::optional<std::string_view> immutable_rom_aot_admission;
      // SEG-047 (ADR 0096): opt-in native ML admission. The frozen v1 model proposes an executable region R; structural pruning derives
      // K; the unchanged hybrid-plan validator decides. Any failure falls back to the broad universe with a stable sanitized reason
      // (stderr `m68k admission:` line). An explicit --immutable-rom-aot-admission plan (exact map) outranks it and excludes it.
      bool immutable_rom_aot_ml_admission = false;
      // SEG-045 (ADR 0094): REPORT-ONLY structural pruning of an externally proposed executable region. The proposal
      // (`segarecomp.m68k_executable_regions.v1`) is pruned to the greatest structurally closed subset of the broad identities and the
      // resulting ordinary hybrid admission plan is written; nothing is generated. Both options are required together.
      // SEG-045: report-only sink for the instruction addresses of the PRECISE direct-control discovery (`FrontendAnalysis::decoded`),
      // one hex address per line, ascending. A region-seed input only; it cannot alter generation.
      std::optional<std::string_view> direct_control_address_report;
      // SEG-046: report-only generic per-window structure features (ADR 0095); both options are required together.
      std::optional<std::string_view> window_feature_report;
      std::uint32_t window_feature_bytes = 0U;
      std::optional<std::string_view> region_proposal_path;
      // SEG-047 (ADR 0096): report-only native ML region proposal (frozen v1 model) written as `segarecomp.m68k_executable_regions.v1`.
      std::optional<std::string_view> ml_region_proposal_output;
      std::optional<std::string_view> region_plan_output;
      // SEG-022-T002: stream the generated C to this file (fail-closed: written as `<path>.partial`
      // and atomically renamed only on complete success; removed on any failure).
      std::optional<std::string_view> generated_c_output;
      // SEG-022-T003: emit the generated C as a bounded deterministic set of translation units in this
      // directory (shared header + main TU + block/AOT/stop/meta/entries TUs + `bridge_generated.units`
      // manifest). Alone it forces sharding; --generated-c-output alone forces the single file; given both,
      // the emitter shards iff the accepted program has >= generated_c_shard_threshold compiled units.
      std::optional<std::string_view> generated_c_shard_dir;
      // SEG-036-T002: the compact direct-entry representation of helper-backed AOT entries is the default for sharded
      // output; --legacy-aot-entries selects the previous owner/wrapper form (differential evidence and bisection).
      bool aot_direct_entries = true;
      for (int index = 4; index < argc;) {
        const std::string_view option = argv[index];
        if (option == "--reset-entry") {
          if (reset_entry) { print_usage(std::cerr); return 2; }
          reset_entry = true; ++index;
        } else if (option == "--immutable-rom-aot") {
          if (immutable_rom_aot) { print_usage(std::cerr); return 2; }
          immutable_rom_aot = true;
          ++index;
        } else if (option == "--immutable-copy-alias") {
          if (index + 1 >= argc) { print_usage(std::cerr); return 2; }
          const std::string_view text = argv[index + 1];
          const auto first = text.find(':');
          const auto second = first == std::string_view::npos ? first : text.find(':', first + 1U);
          if (second == std::string_view::npos) { print_usage(std::cerr); return 2; }
          const auto execution = parse_hex(text.substr(0, first), 8);
          const auto source = parse_hex(text.substr(first + 1U, second - first - 1U), 8);
          const auto length = parse_hex(text.substr(second + 1U), 8);
          if (!execution || !source || !length) { print_usage(std::cerr); return 2; }
          immutable_copy_aliases.push_back({static_cast<std::uint32_t>(*execution),
                                            static_cast<std::uint32_t>(*source),
                                            static_cast<std::uint32_t>(*length)});
          index += 2;
        } else if (option == "--immutable-rom-aot-admission") {
          if (immutable_rom_aot_admission || index + 1 >= argc) { print_usage(std::cerr); return 2; }
          immutable_rom_aot_admission = argv[index + 1];
          index += 2;
        } else if (option == "--immutable-rom-aot-ml-admission") {
          if (immutable_rom_aot_ml_admission) { print_usage(std::cerr); return 2; }
          immutable_rom_aot_ml_admission = true;
          ++index;
        } else if (option == "--direct-control-address-report") {
          if (direct_control_address_report || index + 1 >= argc) { print_usage(std::cerr); return 2; }
          direct_control_address_report = argv[index + 1];
          index += 2;
        } else if (option == "--window-feature-report") {
          if (window_feature_report || index + 1 >= argc) { print_usage(std::cerr); return 2; }
          window_feature_report = argv[index + 1];
          index += 2;
        } else if (option == "--window-feature-bytes") {
          if (window_feature_bytes != 0U || index + 1 >= argc) { print_usage(std::cerr); return 2; }
          const std::string_view value = argv[index + 1];
          if (value == "256") window_feature_bytes = 256U;
          else if (value == "512") window_feature_bytes = 512U;
          else { print_usage(std::cerr); return 2; }
          index += 2;
        } else if (option == "--ml-region-proposal-output") {
          if (ml_region_proposal_output || index + 1 >= argc) { print_usage(std::cerr); return 2; }
          ml_region_proposal_output = argv[index + 1];
          index += 2;
        } else if (option == "--immutable-aot-region-proposal") {
          if (region_proposal_path || index + 1 >= argc) { print_usage(std::cerr); return 2; }
          region_proposal_path = argv[index + 1];
          index += 2;
        } else if (option == "--region-admission-plan-output") {
          if (region_plan_output || index + 1 >= argc) { print_usage(std::cerr); return 2; }
          region_plan_output = argv[index + 1];
          index += 2;
        } else if (option == "--immutable-aot-address-report") {
          if (immutable_aot_address_report || index + 1 >= argc) { print_usage(std::cerr); return 2; }
          immutable_aot_address_report = argv[index + 1];
          index += 2;
        } else if (option == "--generated-c-output") {
          if (generated_c_output || index + 1 >= argc) { print_usage(std::cerr); return 2; }
          generated_c_output = argv[index + 1];
          index += 2;
        } else if (option == "--generated-c-shard-dir") {
          if (generated_c_shard_dir || index + 1 >= argc) { print_usage(std::cerr); return 2; }
          generated_c_shard_dir = argv[index + 1];
          index += 2;
        } else if (option == "--legacy-aot-entries") {
          if (!aot_direct_entries) { print_usage(std::cerr); return 2; }
          aot_direct_entries = false;
          ++index;
        } else if (option == "--provenance-diagnostics") {
          if (provenance_diagnostics) { print_usage(std::cerr); return 2; }
          provenance_diagnostics = true;
          ++index;
        } else if (option == "--entry" || option == "--mapping-base") {
          if (index + 1 >= argc) { print_usage(std::cerr); return 2; }
          const auto value = parse_hex(argv[index + 1], 8);
          if (!value || (option == "--entry" && (*value & 1U) != 0U) ||
              (*value & UINT64_C(0xFF000000)) != 0U) {
            std::cerr << "segarecomp: invalid bridge input\n"; return 2;
          }
          auto &destination = option == "--entry" ? entry_address : mapping_base;
          if (destination) { print_usage(std::cerr); return 2; }
          destination = static_cast<std::uint32_t>(*value); index += 2;
        } else if (option == "--analysis-seed") {
          if (index + 1 >= argc) { print_usage(std::cerr); return 2; }
          const auto value = parse_hex(argv[index + 1], 8);
          if (!value || (*value & 1U) != 0U || (*value & UINT64_C(0xFF000000)) != 0U) {
            std::cerr << "segarecomp: invalid bridge input\n"; return 2;
          }
          analysis_seeds.push_back(static_cast<std::uint32_t>(*value));
          index += 2;
        } else if (option == "--rom-sha256") {
          if (index + 1 >= argc || digest) { print_usage(std::cerr); return 2; }
          digest = argv[index + 1]; index += 2;
        } else if (option == "--synthetic-completion-rts") {
          if (index + 3 >= argc || std::string_view(argv[index + 2]) != "--synthetic-completion-sentinel" ||
              synthetic_completion) { print_usage(std::cerr); return 2; }
          const auto rts = parse_hex(argv[index + 1], 8);
          const auto sentinel = parse_hex(argv[index + 3], 8);
          if (!rts || !sentinel || (*rts & 1U) != 0U || (*sentinel & 1U) != 0U ||
              (*rts & UINT64_C(0xFF000000)) != 0U || (*sentinel & UINT64_C(0xFF000000)) != 0U) {
            std::cerr << "segarecomp: invalid synthetic completion\n"; return 2;
          }
          synthetic_completion = {{static_cast<std::uint32_t>(*rts), static_cast<std::uint32_t>(*sentinel)}};
          index += 4;
        } else if (option == "--external-hints") {
          if (index + 1 >= argc || external_hints_path) { print_usage(std::cerr); return 2; }
          external_hints_path = argv[index + 1]; index += 2;
        } else {
          print_usage(std::cerr); return 2;
        }
      }
      if (immutable_rom_aot_ml_admission && (!immutable_rom_aot || immutable_rom_aot_admission)) {
        std::cerr << "segarecomp: --immutable-rom-aot-ml-admission requires --immutable-rom-aot and excludes --immutable-rom-aot-admission\n";
        return 2;
      }
      if (immutable_rom_aot_admission && !immutable_rom_aot) {
        std::cerr << "segarecomp: --immutable-rom-aot-admission requires --immutable-rom-aot\n"; return 2;
      }
      if (!digest || (reset_entry && (entry_address || mapping_base)) ||
          (!reset_entry && (!entry_address || !mapping_base)) ||
          (!analysis_seeds.empty() && !reset_entry)) {
        print_usage(std::cerr); return 2;
      }
      const std::string_view digest_value = *digest;
      const bool digest_ok = digest_value.size() == 64U && std::all_of(digest_value.begin(), digest_value.end(), [](char value) {
        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
      });
      if (!digest_ok) {
        std::cerr << "segarecomp: invalid bridge input\n"; return 2;
      }
      const auto bytes = segarecomp::read_binary(argv[3]);
      std::optional<segarecomp::FrontendProgram> program;
      if (reset_entry) {
        const auto reset = segarecomp::analyze_genesis_reset_image(bytes);
        if (reset.outcome != segarecomp::ResetOutcome::accepted) {
          std::cerr << segarecomp::format_genesis_reset_image_report(reset) << '\n'; return 1;
        }
        program = segarecomp::make_genesis_reset_bridge_startup_program(
            bytes, reset, synthetic_completion);
      } else {
        program = segarecomp::make_genesis_bridge_startup_program(
            bytes, *mapping_base, *entry_address, synthetic_completion);
      }
      if (!program) { std::cerr << "segarecomp: bridge mapping overflows\n"; return 2; }
      // SEG-007-T164 / ADR-0023: parse the optional, explicitly opted-in
      // external hints file. Any hints file content that does not pass
      // `parse_genesis_external_hints`'s own schema/ROM-hash validation
      // silently contributes zero hints (ADR-0023's fail-closed contract) --
      // this is never a hard CLI error, since a stale or unmatched hints
      // file must never block ordinary recompilation.
      if (external_hints_path) {
        const auto hints_bytes = segarecomp::read_binary(std::string(*external_hints_path));
        const std::string_view hints_text(reinterpret_cast<const char *>(hints_bytes.data()), hints_bytes.size());
        program->external_logical_table_descriptor_hints =
            segarecomp::parse_genesis_external_hints(hints_text, digest_value);
        // Explicit provenance: never silent. Recorded to stderr (never
        // stdout, which carries only the generated C) so the generation
        // session's own transcript/log always shows exactly which trusted,
        // ROM-hash-verified annotations this compilation depended on.
        for (const auto &hint : program->external_logical_table_descriptor_hints) {
          std::cerr << "segarecomp: external hint opted in: logical_table_descriptor base=0x" << std::hex
                     << hint.base_address << std::dec << " entry_width_bytes=" << hint.entry_width_bytes
                     << " stride_bytes=" << hint.stride_bytes << " entry_count=" << hint.entry_count
                     << " provenance_tool=" << (hint.provenance_tool.empty() ? "(unspecified)" : hint.provenance_tool)
                     << '\n';
        }
        // SEG-007-T174 / ADR-0024: the same interchange file may also carry
        // zero or more `code_entry_candidate` records; parsed independently
        // (a different `kind`), same ROM-hash precondition, same fail-closed
        // contract. These are PROPOSALS only -- see `FrontendProgram::
        // external_code_entry_candidates`'s own doc comment -- so this
        // logs their acceptance into the seed set, not a build dependency.
        program->external_code_entry_candidates =
            segarecomp::parse_genesis_external_code_entry_candidates(hints_text, digest_value);
        program->external_code_pointer_table_descriptors =
            segarecomp::parse_genesis_external_code_pointer_table_descriptors(hints_text, digest_value);
        for (const auto &descriptor : program->external_code_pointer_table_descriptors) {
          std::cerr << "segarecomp: external hint opted in: code_pointer_table_descriptor base=0x"
                    << std::hex << descriptor.base_address << std::dec
                    << " entry_width_bytes=4 stride_bytes=4 entry_count=" << descriptor.entry_count
                    << " provenance_tool=" << descriptor.provenance_tool << '\n';
        }
        segarecomp::apply_genesis_code_pointer_table_descriptors(*program);
        // SEG-007-T204 / ADR-0033 (re-homed by the 2026-09-11 operator
        // correction): the same interchange file may also carry zero or more
        // raw, explicitly non-authoritative `address_table_candidate`
        // records. Independent structural corroboration (never the raw
        // candidate alone) decides whether any of them contributes ordinary
        // `code_entry_candidate` proposals -- never a trusted
        // `logical_table_descriptor` -- into `external_code_entry_candidates`
        // below; a candidate that does not corroborate contributes nothing.
        program->external_address_table_candidates =
            segarecomp::parse_genesis_external_address_table_candidates(hints_text, digest_value);
        segarecomp::apply_genesis_address_table_corroboration(*program);
        for (const auto &candidate : program->external_code_entry_candidates) {
          std::cerr << "segarecomp: external hint opted in: code_entry_candidate address=0x" << std::hex
                     << candidate.address << std::dec << " provenance_tool="
                     << (candidate.provenance_tool.empty() ? "(unspecified)" : candidate.provenance_tool) << '\n';
        }
      }
      // Apply the complete source only when explicitly requested. It is
      // independent of external hints and fails closed on malformed or
      // ambiguous immutable mapping claims.
      if (immutable_rom_aot) {
        const bool applied = segarecomp::apply_genesis_immutable_rom_aot(*program);
        if (!applied) {
          std::cerr << "segarecomp: invalid immutable-ROM AOT mapping source\n"; return 2;
        }
        for (const auto &alias : immutable_copy_aliases)
          if (!segarecomp::apply_genesis_immutable_copy_alias(*program, alias[0], alias[1], alias[2])) {
            std::cerr << "segarecomp: invalid immutable-copy alias\n"; return 2;
          }
      } else if (!immutable_copy_aliases.empty()) {
        std::cerr << "segarecomp: --immutable-copy-alias requires --immutable-rom-aot\n"; return 2;
      }
      // ADR-0013 Decision §7 Phase B seed transport: an out-of-mapping seed
      // is not rejected here -- discover_m68k_general_startup's per-seed walk
      // already fails closed on an unmapped/conflicting instruction source
      // for any seed, exactly as it always has for the round-1 entry.
      for (const auto seed : analysis_seeds)
        program->runtime_confirmed_seeds.push_back({segarecomp::TargetAddressSpace::m68k_program, seed});
      const auto calls_of = [](const std::vector<segarecomp::M68kStaticFrame> &frames) {
        std::vector<segarecomp::M68kStaticCall> calls;
        for (const auto &frame : frames) calls.push_back(frame.call);
        return calls;
      };
      auto result = segarecomp::analyze_m68k_frontend(*program);
      if (immutable_rom_aot_admission) {
        std::ifstream plan_file{std::string(*immutable_rom_aot_admission), std::ios::binary};
        std::string plan_text;
        if (plan_file) {
          plan_text.resize(segarecomp::genesis_hybrid_admission_plan_max_bytes + 1U);
          plan_file.read(plan_text.data(), static_cast<std::streamsize>(plan_text.size()));
          plan_text.resize(static_cast<std::size_t>(plan_file.gcount()));
        }
        if (!plan_file && !plan_file.eof()) { std::cerr << "segarecomp: cannot read hybrid admission plan\n"; return 2; }
        std::string parse_error;
        const auto plan = segarecomp::parse_genesis_hybrid_admission_plan(plan_text, &parse_error);
        if (!plan) { std::cerr << "segarecomp: hybrid admission plan rejected: " << parse_error << '\n'; return 2; }
        std::vector<segarecomp::FrontendAnalysis::ImmutableRomAotEntry> *entries = nullptr;
        if (auto *partial = std::get_if<segarecomp::FrontendPartialProgram>(&result)) entries = &partial->accepted_prefix.immutable_rom_aot_entries;
        else if (auto *accepted = std::get_if<segarecomp::FrontendAnalysis>(&result)) entries = &accepted->immutable_rom_aot_entries;
        if (entries != nullptr) {
          const auto broad_count = entries->size();
          if (const auto failure = segarecomp::apply_genesis_hybrid_admission(*program, digest_value, *plan, *entries)) {
            std::cerr << "segarecomp: hybrid admission plan rejected: " << *failure << '\n'; return 2;
          }
          std::cerr << "segarecomp: m68k admission: strategy=" << segarecomp::genesis_admission_strategy_name(plan->strategy)
                    << " admitted=" << entries->size() << " broad=" << broad_count << '\n';
        }
      }
      if (immutable_rom_aot_ml_admission) {
        std::vector<segarecomp::FrontendAnalysis::ImmutableRomAotEntry> *entries = nullptr;
        const segarecomp::FrontendAnalysis *direct = nullptr;
        if (auto *partial = std::get_if<segarecomp::FrontendPartialProgram>(&result)) {
          entries = &partial->accepted_prefix.immutable_rom_aot_entries;
          direct = &partial->accepted_prefix;
        } else if (auto *accepted = std::get_if<segarecomp::FrontendAnalysis>(&result)) {
          entries = &accepted->immutable_rom_aot_entries;
          direct = &*accepted;
        }
        // One machine-readable sanitized line (no address, byte or per-window datum); the build command records it in status.json.
        const auto fallback = [](const std::string &reason, const std::string &detail = "") {
          std::cerr << "segarecomp: m68k admission: requested=optimized producer=broad fallback=1 reason=" << reason
                    << (detail.empty() ? "" : " detail=" + detail) << '\n';
        };
        if (entries == nullptr || direct == nullptr) {
          fallback("no_analysis");
        } else {
          std::vector<std::uint32_t> seeds;
          for (const auto &decoded : direct->decoded)
            seeds.push_back(static_cast<std::uint32_t>(decoded.provenance.source.address.value) & UINT32_C(0x00FFFFFF));
          const auto proposal = segarecomp::propose_genesis_ml_executable_regions(bytes, digest_value, *entries, seeds);
          if (!proposal.proposal) {
            fallback(proposal.failure);
          } else {
            segarecomp::GenesisRegionPruneResult pruned;
            const auto plan = segarecomp::plan_genesis_region_admission(*program, digest_value, *entries, *proposal.proposal, pruned);
            if (!plan) {
              fallback("prune_rejected", pruned.failure_class.empty() ? "unclassified" : pruned.failure_class);
            } else {
              const auto broad_count = entries->size();
              auto scratch = *entries;
              if (segarecomp::apply_genesis_hybrid_admission(*program, digest_value, *plan, scratch).has_value()) {
                fallback("validator_rejected");
              } else {
                *entries = std::move(scratch);  // the validator-accepted, filtered identities (exactly what an explicit plan would admit)
                std::cerr << "segarecomp: m68k admission: requested=optimized producer=ml_region fallback=0 reason=none model="
                          << segarecomp::genesis_ml_feature_version << " schema=d2e7c82913139c29450511326d6de76e91579ec654edde81afae16b13cd1f570"
                          << " windows=" << proposal.stats.windows << " ml_selected=" << proposal.stats.ml_selected
                          << " seed_windows=" << proposal.stats.seed_windows << " universe=" << pruned.universe_count
                          << " k0=" << pruned.k0_count << " k=" << pruned.admitted.size() << " pruned=" << pruned.pruned_count
                          << " rounds=" << pruned.rounds << " broad=" << broad_count << " admitted=" << entries->size()
                          << " k_sha256=" << segarecomp::genesis_hybrid_admission_universe_digest(pruned.admitted) << " validator=accepted\n";
              }
            }
          }
        }
      }
      if (direct_control_address_report) {
        std::vector<std::uint32_t> seen;
        const auto collect_direct = [&seen](const segarecomp::FrontendAnalysis &analysis) {
          for (const auto &decoded : analysis.decoded)
            seen.push_back(static_cast<std::uint32_t>(decoded.provenance.source.address.value) & UINT32_C(0x00FFFFFF));
        };
        if (const auto *partial = std::get_if<segarecomp::FrontendPartialProgram>(&result)) collect_direct(partial->accepted_prefix);
        else if (const auto *accepted = std::get_if<segarecomp::FrontendAnalysis>(&result)) collect_direct(*accepted);
        std::sort(seen.begin(), seen.end());
        seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
        std::ofstream sink{std::string(*direct_control_address_report)};
        for (const auto address : seen) sink << std::hex << address << '\n';
        if (!sink) { std::cerr << "segarecomp: cannot write direct-control address report\n"; return 2; }
      }
      if (window_feature_report.has_value() != (window_feature_bytes != 0U)) { print_usage(std::cerr); return 2; }
      if (window_feature_report) {
        const std::vector<segarecomp::FrontendAnalysis::ImmutableRomAotEntry> *entries = nullptr;
        if (const auto *partial = std::get_if<segarecomp::FrontendPartialProgram>(&result)) entries = &partial->accepted_prefix.immutable_rom_aot_entries;
        else if (const auto *accepted = std::get_if<segarecomp::FrontendAnalysis>(&result)) entries = &accepted->immutable_rom_aot_entries;
        if (entries == nullptr) { std::cerr << "segarecomp: window feature report: no broad analysis result\n"; return 3; }
        const auto report = segarecomp::genesis_window_feature_report(*entries, window_feature_bytes);
        if (!report) { std::cerr << "segarecomp: invalid window size\n"; return 2; }
        std::ofstream sink{std::string(*window_feature_report), std::ios::binary};
        sink << *report;
        if (!sink) { std::cerr << "segarecomp: cannot write window feature report\n"; return 2; }
      }
      if (ml_region_proposal_output) {
        const std::vector<segarecomp::FrontendAnalysis::ImmutableRomAotEntry> *entries = nullptr;
        std::vector<std::uint32_t> seeds;
        const auto collect_seeds = [&seeds](const segarecomp::FrontendAnalysis &analysis) {
          for (const auto &decoded : analysis.decoded)
            seeds.push_back(static_cast<std::uint32_t>(decoded.provenance.source.address.value) & UINT32_C(0x00FFFFFF));
        };
        if (const auto *partial = std::get_if<segarecomp::FrontendPartialProgram>(&result)) {
          entries = &partial->accepted_prefix.immutable_rom_aot_entries;
          collect_seeds(partial->accepted_prefix);
        } else if (const auto *accepted = std::get_if<segarecomp::FrontendAnalysis>(&result)) {
          entries = &accepted->immutable_rom_aot_entries;
          collect_seeds(*accepted);
        }
        if (entries == nullptr) { std::cerr << "segarecomp: ml region proposal: no broad analysis result\n"; return 3; }
        const auto proposal = segarecomp::propose_genesis_ml_executable_regions(bytes, digest_value, *entries, seeds);
        if (!proposal.proposal) { std::cerr << "segarecomp: ml region proposal failed: " << proposal.failure << '\n'; return 3; }
        const auto text = segarecomp::format_genesis_executable_regions(*proposal.proposal);
        std::ofstream sink{std::string(*ml_region_proposal_output), std::ios::binary};
        sink << text;
        if (!sink) { std::cerr << "segarecomp: cannot write ml region proposal\n"; return 2; }
        const auto digest_of = [](const std::string &payload) {
          return segarecomp::sha256_hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(payload.data()), payload.size()));
        };
        std::cerr << "segarecomp: ml region proposal: windows=" << proposal.stats.windows << " ml_selected=" << proposal.stats.ml_selected
                  << " seed_windows=" << proposal.stats.seed_windows << " final_selected=" << proposal.stats.final_selected
                  << " region_bytes=" << proposal.stats.region_bytes << " min_logit_margin=" << proposal.stats.min_logit_margin
                  << " regions_sha256=" << digest_of(text) << " ml_only_regions_sha256="
                  << (proposal.ml_only ? digest_of(segarecomp::format_genesis_executable_regions(*proposal.ml_only)) : std::string("none")) << '\n';
        return 0;
      }
      if (region_proposal_path.has_value() != region_plan_output.has_value() ||
          (region_proposal_path && (!immutable_rom_aot || immutable_rom_aot_admission))) {
        std::cerr << "segarecomp: region proposal requires --immutable-rom-aot, the plan output, and excludes --immutable-rom-aot-admission\n";
        return 2;
      }
      if (region_proposal_path) {
        std::ifstream proposal_file{std::string(*region_proposal_path), std::ios::binary};
        std::string proposal_text;
        if (proposal_file) {
          proposal_text.resize(segarecomp::genesis_hybrid_admission_plan_max_bytes + 1U);
          proposal_file.read(proposal_text.data(), static_cast<std::streamsize>(proposal_text.size()));
          proposal_text.resize(static_cast<std::size_t>(proposal_file.gcount()));
        }
        if (!proposal_file && !proposal_file.eof()) { std::cerr << "segarecomp: cannot read region proposal\n"; return 2; }
        std::string parse_error;
        const auto proposal = segarecomp::parse_genesis_executable_regions(proposal_text, &parse_error);
        if (!proposal) { std::cerr << "segarecomp: region proposal rejected: " << parse_error << '\n'; return 2; }
        const std::vector<segarecomp::FrontendAnalysis::ImmutableRomAotEntry> *entries = nullptr;
        if (const auto *partial = std::get_if<segarecomp::FrontendPartialProgram>(&result)) entries = &partial->accepted_prefix.immutable_rom_aot_entries;
        else if (const auto *accepted = std::get_if<segarecomp::FrontendAnalysis>(&result)) entries = &accepted->immutable_rom_aot_entries;
        if (entries == nullptr) { std::cerr << "segarecomp: region proposal: no broad analysis result\n"; return 3; }
        segarecomp::GenesisRegionPruneResult pruned;
        const auto plan = segarecomp::plan_genesis_region_admission(*program, digest_value, *entries, *proposal, pruned);
        if (!plan) {
          std::cerr << "segarecomp: region proposal REJECTED: " << pruned.failure.value_or("unknown") << (pruned.failure_class.empty() ? "" : " class=" + pruned.failure_class) << " universe=" << pruned.universe_count
                    << " k0=" << pruned.k0_count << " rounds=" << pruned.rounds << '\n';
          return 3;
        }
        // The unchanged production validator must accept the plan on a scratch copy (defence in depth).
        auto scratch = *entries;
        if (const auto failure = segarecomp::apply_genesis_hybrid_admission(*program, digest_value, *plan, scratch)) {
          std::cerr << "segarecomp: region plan rejected by the production validator: " << *failure << '\n';
          return 3;
        }
        std::ofstream sink{std::string(*region_plan_output), std::ios::binary};
        sink << segarecomp::format_genesis_hybrid_admission_plan(*plan);
        if (!sink) { std::cerr << "segarecomp: cannot write region admission plan\n"; return 2; }
        std::cerr << "segarecomp: region prune: universe=" << pruned.universe_count << " k0=" << pruned.k0_count
                  << " k=" << pruned.admitted.size() << " pruned=" << pruned.pruned_count << " rounds=" << pruned.rounds
                  << " validator=accepted ranges=" << plan->ranges.size() << '\n';
        return 0;
      }
      if (immutable_rom_aot) {
        std::uint64_t aligned_start_count = 0U;
        for (const auto &range : program->immutable_rom_aot_ranges)
          aligned_start_count += (static_cast<std::uint64_t>(range.end_address) - range.begin_address + 1U) / 2U;
        std::uint64_t accepted_count = 0U;
        if (immutable_aot_address_report) {
          std::vector<std::uint32_t> admitted;
          const auto collect = [&admitted](const auto &entries) {
            for (const auto &entry : entries)
              admitted.push_back(static_cast<std::uint32_t>(entry.decoded.provenance.source.address.value));
          };
          if (const auto *partial = std::get_if<segarecomp::FrontendPartialProgram>(&result))
            collect(partial->accepted_prefix.immutable_rom_aot_entries);
          else if (const auto *accepted = std::get_if<segarecomp::FrontendAnalysis>(&result))
            collect(accepted->immutable_rom_aot_entries);
          std::sort(admitted.begin(), admitted.end());
          std::ofstream sink{std::string(*immutable_aot_address_report)};
          for (const auto address : admitted) sink << std::hex << address << '\n';
          if (!sink) { std::cerr << "segarecomp: cannot write immutable-AOT address report\n"; return 2; }
        }
        if (const auto *partial = std::get_if<segarecomp::FrontendPartialProgram>(&result))
          accepted_count = partial->accepted_prefix.immutable_rom_aot_entries.size();
        else if (const auto *accepted = std::get_if<segarecomp::FrontendAnalysis>(&result))
          accepted_count = accepted->immutable_rom_aot_entries.size();
        std::cerr << "segarecomp: immutable-rom AOT enumeration: aligned_start_count=" << aligned_start_count
                  << " accepted_count=" << accepted_count
                  << " rejected_count=" << (aligned_start_count - accepted_count) << '\n';
      }
      // Given alone, --generated-c-shard-dir always shards. Given together with --generated-c-output the
      // emitter shards only a program with at least `shard_threshold` compiled units (ordinary blocks +
      // immutable-ROM AOT entries) and otherwise writes the single --generated-c-output file: a pure,
      // deterministic function of the accepted program size, so small programs keep the historical
      // one-file artifact and one giant TU is never produced for a large one.
      const segarecomp::ImmutableRomAotDirectEntriesScope direct_entries_scope(aot_direct_entries);
      std::size_t program_units = 0U;
      if (const auto *partial = std::get_if<segarecomp::FrontendPartialProgram>(&result))
        program_units = segarecomp::generated_program_unit_count(*partial);
      else if (const auto *accepted = std::get_if<segarecomp::FrontendAnalysis>(&result))
        program_units = segarecomp::generated_program_unit_count(*accepted);
      if (segarecomp::select_sharded_generated_c(generated_c_output.has_value(), generated_c_shard_dir.has_value(), program_units)) {
        // Layout (bounded, documented in ADR-0044/ADR-0045): see genesis_bridge_translation_unit_families().
        segarecomp::TranslationUnitSharder sharder{
            std::filesystem::path{std::string(*generated_c_shard_dir)}, "bridge_generated",
            segarecomp::genesis_bridge_translation_unit_families()};
        auto &sink = sharder.stream();
        std::string rejection;
        bool emitted = false;
        if (const auto *partial = std::get_if<segarecomp::FrontendPartialProgram>(&result)) {
          rejection = segarecomp::emit_m68k_general_startup_bridge_c_to(sink, *partial, digest_value, provenance_diagnostics);
          if (rejection.empty() && provenance_diagnostics) {
            const auto &a = partial->accepted_prefix;
            sink << segarecomp::emit_m68k_provenance_diagnostic_c(segarecomp::build_m68k_provenance_diagnostic_projection(
                digest_value, a.decoded, a.static_blocks, a.static_edges, calls_of(a.static_frames)));
          }
          emitted = true;
        } else if (const auto *accepted = std::get_if<segarecomp::FrontendAnalysis>(&result)) {
          rejection = segarecomp::emit_m68k_general_startup_bridge_c_to(sink, *accepted, digest_value, provenance_diagnostics);
          if (rejection.empty() && provenance_diagnostics)
            sink << segarecomp::emit_m68k_provenance_diagnostic_c(segarecomp::build_m68k_provenance_diagnostic_projection(
                digest_value, accepted->decoded, accepted->static_blocks, accepted->static_edges, calls_of(accepted->static_frames)));
          emitted = true;
        }
        if (!emitted) { std::cerr << segarecomp::format_m68k_frontend_result(result) << '\n'; return 1; }
        if (!rejection.empty()) { std::cerr << rejection; return 1; }
        if (!sink) { std::cerr << "segarecomp: generated-C write failed\n"; return 2; }
        if (const auto failure = sharder.finish(); !failure.empty()) { std::cerr << "segarecomp: " << failure << '\n'; return 2; }
        std::cerr << "segarecomp: generated-C translation units: count=" << sharder.translation_unit_count()
                  << " max=" << sharder.max_translation_unit_count() << '\n';
        return 0;
      }
      if (generated_c_output) {
        const std::filesystem::path final_path{std::string(*generated_c_output)};
        auto partial_path = final_path; partial_path += ".partial";
        std::error_code ignored;
        std::filesystem::remove(final_path, ignored);
        const auto fail = [&](int code) { std::filesystem::remove(partial_path, ignored); return code; };
        std::string rejection;
        bool emitted = false;
        {
          std::ofstream file{partial_path, std::ios::binary | std::ios::trunc};
          if (!file) { std::cerr << "segarecomp: cannot open generated-C output\n"; return 2; }
          if (const auto *partial = std::get_if<segarecomp::FrontendPartialProgram>(&result)) {
            rejection = segarecomp::emit_m68k_general_startup_bridge_c_to(file, *partial, digest_value, provenance_diagnostics);
            if (rejection.empty() && provenance_diagnostics) {
              const auto &a = partial->accepted_prefix;
              file << segarecomp::emit_m68k_provenance_diagnostic_c(segarecomp::build_m68k_provenance_diagnostic_projection(
                  digest_value, a.decoded, a.static_blocks, a.static_edges, calls_of(a.static_frames)));
            }
            emitted = true;
          } else if (const auto *accepted = std::get_if<segarecomp::FrontendAnalysis>(&result)) {
            rejection = segarecomp::emit_m68k_general_startup_bridge_c_to(file, *accepted, digest_value, provenance_diagnostics);
            if (rejection.empty() && provenance_diagnostics)
              file << segarecomp::emit_m68k_provenance_diagnostic_c(segarecomp::build_m68k_provenance_diagnostic_projection(
                  digest_value, accepted->decoded, accepted->static_blocks, accepted->static_edges, calls_of(accepted->static_frames)));
            emitted = true;
          }
          if (emitted && rejection.empty()) { file.flush(); if (!file) { std::cerr << "segarecomp: generated-C write failed\n"; return fail(2); } }
        }
        if (!emitted) { std::cerr << segarecomp::format_m68k_frontend_result(result) << '\n'; return fail(1); }
        if (!rejection.empty()) { std::cerr << rejection; return fail(1); }
        std::filesystem::rename(partial_path, final_path, ignored);
        if (ignored) { std::cerr << "segarecomp: cannot finalize generated-C output\n"; return fail(2); }
        return 0;
      }
      if (const auto *partial = std::get_if<segarecomp::FrontendPartialProgram>(&result)) {
        std::cout << segarecomp::emit_m68k_general_startup_bridge_c(*partial, digest_value, provenance_diagnostics);
        if (provenance_diagnostics) {
          const auto &a = partial->accepted_prefix;
          std::cout << segarecomp::emit_m68k_provenance_diagnostic_c(segarecomp::build_m68k_provenance_diagnostic_projection(
              digest_value, a.decoded, a.static_blocks, a.static_edges, calls_of(a.static_frames)));
        }
        return 0;
      }
      if (const auto *accepted = std::get_if<segarecomp::FrontendAnalysis>(&result)) {
        std::cout << segarecomp::emit_m68k_general_startup_bridge_c(*accepted, digest_value, provenance_diagnostics);
        if (provenance_diagnostics)
          std::cout << segarecomp::emit_m68k_provenance_diagnostic_c(segarecomp::build_m68k_provenance_diagnostic_projection(
              digest_value, accepted->decoded, accepted->static_blocks, accepted->static_edges, calls_of(accepted->static_frames)));
        return 0;
      }
      std::cerr << segarecomp::format_m68k_frontend_result(result) << '\n';
      return 1;
    }
    // SEG-026-T001 (experiment, report-only): reachability-first discovery challenger. Writes aggregate JSON to
    // stdout (counts only) and exact PCs to --private-output / --classify-output. The CLI does not police those
    // paths: for a commercial input the caller must place them in an ignored location and never persist them. Reads the image,
    // its reset handoff and optional ADR 0049 alias descriptors only; it never alters generation. --universe also
    // reports the unchanged broad immutable-ROM AOT identity count U for comparison.
    if (command == "genesis-reachability-challenger") {
      std::optional<std::string_view> rom;
      std::optional<std::string_view> digest;
      std::optional<std::string_view> private_output;
      std::optional<std::uint32_t> entry_address;
      std::optional<std::uint32_t> mapping_base;
      bool reset_entry = false;
      bool universe = false;
      // Classification only: labels caller-supplied PCs (e.g. privately observed ones) by generic control
      // mechanism with the same decoder. The result never enters the challenger's discovered set.
      std::optional<std::string_view> classify_input;
      std::optional<std::string_view> classify_output;
      segarecomp::GenesisReachabilityChallengerConfig config{};
      std::vector<std::array<std::uint32_t, 3>> aliases;
      for (int index = 2; index < argc;) {
        const std::string_view option = argv[index];
        const bool has_value = index + 1 < argc;
        if (option == "--rom" && has_value && !rom) { rom = argv[index + 1]; index += 2; }
        else if (option == "--rom-sha256" && has_value && !digest) { digest = argv[index + 1]; index += 2; }
        else if (option == "--private-output" && has_value && !private_output) { private_output = argv[index + 1]; index += 2; }
        else if (option == "--reset-entry" && !reset_entry) { reset_entry = true; ++index; }
        else if (option == "--universe" && !universe) { universe = true; ++index; }
        else if (option == "--classify-pcs" && has_value && !classify_input) { classify_input = argv[index + 1]; index += 2; }
        else if (option == "--classify-output" && has_value && !classify_output) { classify_output = argv[index + 1]; index += 2; }
        else if (option == "--pea-continuations" && !config.pea_continuations) { config.pea_continuations = true; ++index; }
        else if (option == "--pc-index-recovery" && !config.pc_index_recovery) { config.pc_index_recovery = true; ++index; }
        else if (option == "--pc-index-width-domains" && !config.pc_index_width_domains) { config.pc_index_width_domains = true; ++index; }
        else if (option == "--exception-model" && has_value) {
          const std::string_view model = argv[index + 1];
          if (model == "strict") config.exception_model = segarecomp::GenesisReachabilityExceptionModel::strict;
          else if (model == "normal-resumption") config.exception_model = segarecomp::GenesisReachabilityExceptionModel::normal_resumption;
          else { print_usage(std::cerr); return 2; }
          index += 2;
        } else if ((option == "--entry" || option == "--mapping-base") && has_value) {
          const auto value = parse_hex(argv[index + 1], 8);
          auto &destination = option == "--entry" ? entry_address : mapping_base;
          if (!value || destination || (*value & UINT64_C(0xFF000000)) != 0U || (option == "--entry" && (*value & 1U) != 0U)) {
            std::cerr << "segarecomp: invalid challenger input\n"; return 2;
          }
          destination = static_cast<std::uint32_t>(*value);
          index += 2;
        } else if (option == "--immutable-copy-alias" && has_value) {
          const std::string_view text = argv[index + 1];
          const auto first = text.find(':');
          const auto second = first == std::string_view::npos ? first : text.find(':', first + 1U);
          if (second == std::string_view::npos) { print_usage(std::cerr); return 2; }
          const auto execution = parse_hex(text.substr(0, first), 8);
          const auto source = parse_hex(text.substr(first + 1U, second - first - 1U), 8);
          const auto length = parse_hex(text.substr(second + 1U), 8);
          if (!execution || !source || !length) { print_usage(std::cerr); return 2; }
          aliases.push_back({static_cast<std::uint32_t>(*execution), static_cast<std::uint32_t>(*source),
                             static_cast<std::uint32_t>(*length)});
          index += 2;
        } else { print_usage(std::cerr); return 2; }
      }
      if (classify_input.has_value() != classify_output.has_value()) { print_usage(std::cerr); return 2; }
      if (config.pc_index_width_domains && !config.pc_index_recovery) { print_usage(std::cerr); return 2; }
      if (!rom || !digest || !private_output || (reset_entry == (entry_address.has_value() || mapping_base.has_value())) ||
          (!reset_entry && (!entry_address || !mapping_base))) {
        print_usage(std::cerr); return 2;
      }
      const auto bytes = segarecomp::read_binary(std::string(*rom));
      std::optional<segarecomp::FrontendProgram> program;
      if (reset_entry) {
        const auto reset = segarecomp::analyze_genesis_reset_image(bytes);
        if (reset.outcome != segarecomp::ResetOutcome::accepted) {
          std::cerr << segarecomp::format_genesis_reset_image_report(reset) << '\n'; return 1;
        }
        program = segarecomp::make_genesis_reset_bridge_startup_program(bytes, reset, std::nullopt);
      } else {
        program = segarecomp::make_genesis_bridge_startup_program(bytes, *mapping_base, *entry_address, std::nullopt);
      }
      if (!program) { std::cerr << "segarecomp: bridge mapping overflows\n"; return 2; }
      // ADR 0049 aliases are machine mapping facts, recorded through the same fail-closed owner production uses.
      if (!aliases.empty() && !segarecomp::apply_genesis_immutable_rom_aot(*program)) {
        std::cerr << "segarecomp: invalid immutable-ROM AOT mapping source\n"; return 2;
      }
      for (const auto &alias : aliases)
        if (!segarecomp::apply_genesis_immutable_copy_alias(*program, alias[0], alias[1], alias[2])) {
          std::cerr << "segarecomp: invalid immutable-copy alias\n"; return 2;
        }
      const auto result = segarecomp::run_genesis_reachability_challenger(*program, config);
      std::string aggregate = segarecomp::format_genesis_reachability_challenger_aggregate(result, config);
      if (universe) {
        // U: the unchanged broad Gen-2 analysis on an independent copy of the program (never an input to D).
        auto broad = *program;
        if (aliases.empty() && !segarecomp::apply_genesis_immutable_rom_aot(broad)) {
          std::cerr << "segarecomp: invalid immutable-ROM AOT mapping source\n"; return 2;
        }
        const auto analysis = segarecomp::analyze_m68k_frontend(broad);
        std::size_t count = 0U;
        if (const auto *partial = std::get_if<segarecomp::FrontendPartialProgram>(&analysis))
          count = partial->accepted_prefix.immutable_rom_aot_entries.size();
        else if (const auto *accepted = std::get_if<segarecomp::FrontendAnalysis>(&analysis))
          count = accepted->immutable_rom_aot_entries.size();
        else { std::cerr << "segarecomp: broad analysis rejected\n"; return 1; }
        aggregate.insert(aggregate.size() - 1U, ",\"universe_immutable_rom_aot\":" + std::to_string(count));
      }
      if (classify_input) {
        std::ifstream pcs_file{std::string(*classify_input)};
        if (!pcs_file) { std::cerr << "segarecomp: cannot read classify PCs\n"; return 2; }
        std::vector<std::uint32_t> pcs;
        std::string line;
        while (std::getline(pcs_file, line)) {
          if (line.empty()) continue;
          const auto value = parse_hex(line, line.size());
          if (!value || *value > UINT64_C(0xFFFFFFFF)) { std::cerr << "segarecomp: invalid classify PC\n"; return 2; }
          pcs.push_back(static_cast<std::uint32_t>(*value));
        }
        std::ofstream classified{std::string(*classify_output), std::ios::binary};
        classified << segarecomp::format_genesis_reachability_classification_private(
            segarecomp::classify_genesis_reachability_pcs(*program, pcs));
        if (!classified) { std::cerr << "segarecomp: cannot write classification\n"; return 2; }
      }
      std::string private_report = segarecomp::format_genesis_reachability_challenger_private(result, config);
      if (universe) {
        // Carry U into the private report's aggregate too, so the comparison tool can form D/U and O/U: the
        // private report embeds the exact aggregate string, which is replaced by the U-carrying one.
        const auto plain = segarecomp::format_genesis_reachability_challenger_aggregate(result, config);
        const auto at = private_report.find("\"aggregate\":" + plain);
        if (at == std::string::npos) { std::cerr << "segarecomp: malformed challenger report\n"; return 2; }
        private_report.replace(at + 12U, plain.size(), aggregate);
      }
      std::ofstream sink{std::string(*private_output), std::ios::binary};
      sink << private_report;
      if (!sink) { std::cerr << "segarecomp: cannot write challenger private output\n"; return 2; }
      std::cout << aggregate << '\n';
      return 0;
    }
    if (command == "emit-genesis-pc-relative-offset-table-proposals") {
      // SEG-007-T206 / Scope Phase 2: mechanical, non-authoritative proposal
      // detection for a `(d8,PC,Xn)` word-Dn-indexed control site whose
      // table base has no matching ADR-0023 `logical_table_descriptor`. A
      // narrow CLI surface covering exactly the canonical `--reset-entry`
      // one-shot commercial route -- the same program-construction and
      // `--external-hints` loading rule `emit-general-startup-bridge-c`
      // already uses for that same route -- so the existing-descriptor set
      // this command filters against is the same one the canonical route
      // itself would consume.
      if (argc < 7 || std::string_view(argv[2]) != "--rom") { print_usage(std::cerr); return 2; }
      bool reset_entry = false;
      std::optional<std::string_view> digest;
      std::optional<std::string_view> external_hints_path;
      for (int index = 4; index < argc;) {
        const std::string_view option = argv[index];
        if (option == "--reset-entry") {
          if (reset_entry) { print_usage(std::cerr); return 2; }
          reset_entry = true; ++index;
        } else if (option == "--rom-sha256") {
          if (index + 1 >= argc || digest) { print_usage(std::cerr); return 2; }
          digest = argv[index + 1]; index += 2;
        } else if (option == "--external-hints") {
          if (index + 1 >= argc || external_hints_path) { print_usage(std::cerr); return 2; }
          external_hints_path = argv[index + 1]; index += 2;
        } else {
          print_usage(std::cerr); return 2;
        }
      }
      if (!reset_entry || !digest) { print_usage(std::cerr); return 2; }
      const std::string_view digest_value = *digest;
      const bool digest_ok = digest_value.size() == 64U && std::all_of(digest_value.begin(), digest_value.end(), [](char value) {
        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
      });
      if (!digest_ok) { std::cerr << "segarecomp: invalid bridge input\n"; return 2; }
      const auto bytes = segarecomp::read_binary(argv[3]);
      const auto reset = segarecomp::analyze_genesis_reset_image(bytes);
      if (reset.outcome != segarecomp::ResetOutcome::accepted) {
        std::cerr << segarecomp::format_genesis_reset_image_report(reset) << '\n'; return 1;
      }
      auto program = segarecomp::make_genesis_reset_bridge_startup_program(bytes, reset, std::nullopt);
      if (!program) { std::cerr << "segarecomp: bridge mapping overflows\n"; return 2; }
      if (external_hints_path) {
        // Mirrors `emit-general-startup-bridge-c`'s own external-hints
        // loading exactly (every recognized record kind, every existing
        // promotion function) -- the same reachable-ROM surface (ADR-0025's
        // whole-ROM offline stitching driven by `external_code_entry_
        // candidates`) the canonical one-shot route itself walks, so this
        // command observes the identical set of unresolved Tier-2 facts.
        const auto hints_bytes = segarecomp::read_binary(std::string(*external_hints_path));
        const std::string_view hints_text(reinterpret_cast<const char *>(hints_bytes.data()), hints_bytes.size());
        program->external_logical_table_descriptor_hints =
            segarecomp::parse_genesis_external_hints(hints_text, digest_value);
        program->external_code_entry_candidates =
            segarecomp::parse_genesis_external_code_entry_candidates(hints_text, digest_value);
        program->external_code_pointer_table_descriptors =
            segarecomp::parse_genesis_external_code_pointer_table_descriptors(hints_text, digest_value);
        segarecomp::apply_genesis_code_pointer_table_descriptors(*program);
        program->external_address_table_candidates =
            segarecomp::parse_genesis_external_address_table_candidates(hints_text, digest_value);
        segarecomp::apply_genesis_address_table_corroboration(*program);
      }
      const auto result = segarecomp::analyze_m68k_frontend(*program);
      const std::vector<segarecomp::M68kUnprovenIndirectControlEaSet> *unproven = nullptr;
      if (const auto *partial = std::get_if<segarecomp::FrontendPartialProgram>(&result)) {
        unproven = &partial->accepted_prefix.unproven_indirect_control_ea_sets;
      } else if (const auto *accepted = std::get_if<segarecomp::FrontendAnalysis>(&result)) {
        unproven = &accepted->unproven_indirect_control_ea_sets;
      } else {
        std::cerr << segarecomp::format_m68k_frontend_result(result) << '\n'; return 1;
      }
      const auto proposals = segarecomp::detect_genesis_pc_relative_offset_table_extent_proposals(
          *unproven, program->external_logical_table_descriptor_hints, "segarecomp-static-discovery",
          "SEG-007-T206", "1970-01-01T00:00:00Z");
      std::cout << segarecomp::serialize_genesis_pc_relative_offset_table_extent_proposals(proposals, digest_value);
      return 0;
    }
    if (command == "genesis-rom-startup" || command == "emit-genesis-rom-startup-c" ||
        command == "genesis-general-startup") {
      if (argc != 3) { print_usage(std::cerr); return 2; }
      const auto bytes = segarecomp::read_binary(argv[2]);
      // Preserve the accepted classification and reset-vector ingress before
      // constructing any startup frontend record.
      const auto info = segarecomp::inspect_rom(bytes);
      if (info.outcome != segarecomp::ClassificationOutcome::recognized ||
          info.platform != segarecomp::Platform::genesis) {
        std::cerr << "segarecomp: " << segarecomp::diagnostic_name(info.diagnostic) << '\n'; return 1;
      }
      const auto reset = segarecomp::analyze_genesis_reset_image(bytes);
      if (reset.outcome != segarecomp::ResetOutcome::accepted) {
        std::cerr << segarecomp::format_genesis_reset_image_report(reset) << '\n'; return 2;
      }
      const auto initial = segarecomp::make_genesis_reset_startup_state(reset);
      const auto program = segarecomp::make_genesis_reset_startup_program(
          bytes, initial, command == "genesis-general-startup");
      const auto analysis = segarecomp::analyze_m68k_frontend(program);
      if (!std::holds_alternative<segarecomp::FrontendAnalysis>(analysis)) {
        std::cerr << segarecomp::format_m68k_frontend_result(analysis) << '\n'; return 1;
      }
      if (command == "genesis-general-startup") {
        std::cout << segarecomp::format_m68k_general_startup_result(
                         std::get<segarecomp::FrontendAnalysis>(analysis))
                  << '\n'; return 0;
      }
      const auto result = segarecomp::execute_m68k_frontend_startup(
          std::get<segarecomp::FrontendAnalysis>(analysis), initial);
      if (std::holds_alternative<segarecomp::StartupFailure>(result)) {
        std::cerr << segarecomp::format_genesis_rom_startup_result(result) << '\n'; return 1;
      }
      if (command == "emit-genesis-rom-startup-c") {
        segarecomp::DirectFlowState emission_initial{};
        emission_initial.d = initial.d; emission_initial.sr = initial.sr; emission_initial.pc = initial.pc;
        std::cout << segarecomp::emit_m68k_frontend_c(std::get<segarecomp::FrontendAnalysis>(analysis), emission_initial, 5U);
      }
      else std::cout << segarecomp::format_genesis_rom_startup_result(result) << '\n';
      return 0;
    }
    if (command == "probe-genesis-startup-decode") {
      // A generic, profile-neutral probe over the existing shared decoder;
      // it recognizes zero new instruction forms of its own.
      //
      if (argc != 4) { print_usage(std::cerr); return 2; }
      const auto primary = parse_hex(argv[2], 4);
      if (!primary) { print_usage(std::cerr); return 2; }
      const std::string_view extension_arg = argv[3];
      std::vector<std::uint8_t> bytes{static_cast<std::uint8_t>((*primary >> 8U) & 0xFFU),
                                       static_cast<std::uint8_t>(*primary & 0xFFU)};
      if (extension_arg != "-") {
        const auto extension = parse_hex(extension_arg, 8);
        if (!extension) { print_usage(std::cerr); return 2; }
        bytes.push_back(static_cast<std::uint8_t>((*extension >> 24U) & 0xFFU));
        bytes.push_back(static_cast<std::uint8_t>((*extension >> 16U) & 0xFFU));
        bytes.push_back(static_cast<std::uint8_t>((*extension >> 8U) & 0xFFU));
        bytes.push_back(static_cast<std::uint8_t>(*extension & 0xFFU));
      }
      const segarecomp::DecodeSource source{segarecomp::CpuVariant::mc68000,
          {segarecomp::TargetAddressSpace::m68k_program, 0U}, segarecomp::MoveqImageOffset{0U}};
      const auto result = segarecomp::decode_m68k_instruction(bytes, source,
          segarecomp::M68kDecodeProfile::genesis_startup);
      if (const auto *decoded = std::get_if<segarecomp::M68kDecodedInstruction>(&result)) {
        // CPU decode/lift and C11 lowering jointly own the compatibility
        // support assessment; the CLI only presents their result.
        const auto ir = segarecomp::lift_m68k_instruction(*decoded);
        const bool partially_supported = !segarecomp::m68k_operation_has_complete_c_emission(ir);
        std::cout << "{\"decoded\":true,\"kind\":\"" << segarecomp::m68k_probe_instruction_kind_name(*decoded)
                   << "\",\"length\":" << decoded->provenance.length.value << ",\"support\":\""
                   << (partially_supported ? "partially_supported" : "supported") << "\"}\n";
        return 0;
      }
      const auto &rejected = std::get<segarecomp::RejectedM68kDecode>(result);
      std::cout << "{\"decoded\":false,\"outcome\":\"" << segarecomp::decode_outcome_name(rejected.outcome)
                 << "\",\"unsupported_instruction_form\":"
                 << (rejected.unsupported_instruction_form ? "true" : "false")
                 << ",\"support\":\"unsupported\"}\n";
      return 0;
    }
    if (command == "probe-genesis-startup-mapping") {
      // Classifies against exactly the two selected mapping facts
      // (raw_cartridge_rom's [0, image_length) construction from
      // genesis-rom-startup, and the shared synthetic-work-RAM window
      // predicate); it invents no third mapping name. Classification is
      // always whole-range membership: a crossing/partially-out-of-range
      // access (address inside a mapping but address+width outside it)
      // satisfies neither predicate and correctly falls through to
      // hardware_frontier rather than being silently accepted.
      if (argc != 5) { print_usage(std::cerr); return 2; }
      const auto address = parse_hex(argv[2], 8);
      std::uint32_t width{};
      const std::string_view width_arg = argv[3];
      const auto width_result = std::from_chars(width_arg.data(), width_arg.data() + width_arg.size(), width, 10);
      const bool width_ok = width_result.ec == std::errc{} &&
                             width_result.ptr == width_arg.data() + width_arg.size() && width != 0U;
      const auto image_length = parse_hex(argv[4], 16);
      if (!address || !width_ok || !image_length) { print_usage(std::cerr); return 2; }
      const auto category = segarecomp::classify_genesis_startup_mapping(*address, width, *image_length);
      std::cout << "{\"category\":\"" << segarecomp::genesis_startup_mapping_class_name(category) << "\"}\n";
      return 0;
    }
    if (command == "m68k-frontend" || command == "emit-m68k-frontend-c") {
      const bool emit = command == "emit-m68k-frontend-c";
      const int fixed = emit ? 16 : 5;
      if (argc < fixed + 5 || ((argc - fixed) % 5) != 0) { print_usage(std::cerr); return 2; }
      segarecomp::FrontendProgram program{};
      program.image.bytes = segarecomp::read_binary(argv[2]);
      program.image.source_id = argv[3]; program.image.byte_length = program.image.bytes.size();
      const auto entry = parse_hex(argv[4], 8); if (!entry) { std::cerr << "segarecomp: invalid frontend entry\n"; return 2; }
      program.analysis_entries.push_back({segarecomp::TargetAddressSpace::m68k_program, static_cast<std::uint32_t>(*entry)});
      segarecomp::DirectFlowState initial{}; initial.pc = program.analysis_entries.front();
      std::uint64_t budget{};
      int claims = 5;
      if (emit) {
        const auto execution_entry = parse_hex(argv[5], 8); const auto sr = parse_hex(argv[6], 4); const auto parsed_budget = parse_hex(argv[7], 16);
        if (!execution_entry || !sr || !parsed_budget || *parsed_budget == 0U) { std::cerr << "segarecomp: invalid frontend state\n"; return 2; }
        initial.pc.value = static_cast<std::uint32_t>(*execution_entry); initial.sr = static_cast<std::uint16_t>(*sr); budget = *parsed_budget;
        for (std::size_t i=0;i<initial.d.size();++i) { const auto value=parse_hex(argv[8+static_cast<int>(i)],8); if(!value){std::cerr<<"segarecomp: invalid frontend state\n";return 2;} initial.d[i]=static_cast<std::uint32_t>(*value); }
        claims = 16;
      }
      for (int index=claims; index<argc; index+=5) {
        const auto begin=parse_hex(argv[index+1],8); const auto end=parse_hex(argv[index+2],8); const auto image_begin=parse_hex(argv[index+3],16); const auto image_end=parse_hex(argv[index+4],16);
        if(!begin||!end||!image_begin||!image_end){std::cerr<<"segarecomp: invalid frontend mapping\n";return 2;}
        program.mapping_claims.push_back({argv[index], {segarecomp::TargetAddressSpace::m68k_program,static_cast<std::uint32_t>(*begin)}, {segarecomp::TargetAddressSpace::m68k_program,static_cast<std::uint32_t>(*end)}, {*image_begin}, {*image_end}});
      }
      const auto result=segarecomp::analyze_m68k_frontend(program);
      if(std::holds_alternative<segarecomp::FrontendRejected>(result)){std::cerr<<segarecomp::format_m68k_frontend_result(result)<<'\n';return 1;}
      if(emit) std::cout<<segarecomp::emit_m68k_frontend_c(std::get<segarecomp::FrontendAnalysis>(result),initial,budget); else std::cout<<segarecomp::format_m68k_frontend_result(result)<<'\n';
      return 0;
    }
    if (command == "emit-moveq-c") {
      if (argc != 14) { print_usage(std::cerr); return 2; }
      const auto pc = parse_hex(argv[3], 8); const auto offset = parse_hex(argv[4], 16); const auto sr = parse_hex(argv[5], 4);
      if (!pc || !offset || !sr) { std::cerr << "segarecomp: invalid MOVEQ state\n"; return 2; }
      segarecomp::MoveqCpuState state{};
      state.pc.value = static_cast<std::uint32_t>(*pc); state.sr = static_cast<std::uint16_t>(*sr);
      for (std::size_t index = 0; index < state.d.size(); ++index) {
        const auto value = parse_hex(argv[6 + static_cast<int>(index)], 8);
        if (!value) { std::cerr << "segarecomp: invalid MOVEQ state\n"; return 2; }
        state.d[index] = static_cast<std::uint32_t>(*value);
      }
      const auto bytes = segarecomp::read_binary(argv[2]);
      const segarecomp::DecodeSource source{segarecomp::CpuVariant::mc68000, state.pc, segarecomp::MoveqImageOffset{*offset}};
      const auto decoded = segarecomp::decode_moveq(bytes, source);
      if (const auto *rejected = std::get_if<segarecomp::RejectedMoveq>(&decoded)) {
        std::cerr << segarecomp::format_moveq_rejection(*rejected) << '\n';
        return 1;
      }
      std::cout << segarecomp::emit_moveq_c(segarecomp::lift_moveq(std::get<segarecomp::DecodedMoveq>(decoded).instruction), state);
      return 0;
    }
    if (command == "emit-direct-flow-c") {
      if (argc != 15) { print_usage(std::cerr); return 2; }
      const auto pc=parse_hex(argv[3],8); const auto offset=parse_hex(argv[4],16); const auto sr=parse_hex(argv[5],4); const auto budget=parse_hex(argv[6],16);
      if(!pc||!offset||!sr||!budget||*budget==0U) { std::cerr << "segarecomp: invalid direct-flow state\n"; return 2; }
      segarecomp::DirectFlowState state{}; state.pc.value=static_cast<std::uint32_t>(*pc); state.sr=static_cast<std::uint16_t>(*sr);
      for(std::size_t i=0;i<state.d.size();++i) { const auto value=parse_hex(argv[7+static_cast<int>(i)],8); if(!value) { std::cerr<<"segarecomp: invalid direct-flow state\n";return 2;} state.d[i]=static_cast<std::uint32_t>(*value); }
      const auto bytes=segarecomp::read_binary(argv[2]);
      if (*offset > state.pc.value || bytes.size() > UINT32_MAX - (state.pc.value - static_cast<std::uint32_t>(*offset))) { std::cerr << "segarecomp: invalid direct-flow mapping\n"; return 2; }
      segarecomp::DirectFlowProgram program{}; program.reset_entry=state.pc;
      const auto mapping_begin = static_cast<std::uint32_t>(state.pc.value - static_cast<std::uint32_t>(*offset));
      program.mappings.push_back({"image", {segarecomp::TargetAddressSpace::m68k_program,mapping_begin}, {segarecomp::TargetAddressSpace::m68k_program,static_cast<std::uint32_t>(mapping_begin+bytes.size())}, {0}, {bytes.size()}});
      const auto discovered=segarecomp::discover_direct_flow(bytes,program);
      if(const auto *rejected=std::get_if<segarecomp::RejectedDirectFlow>(&discovered)) { std::cerr<<segarecomp::format_direct_flow_rejection(*rejected)<<'\n';return 1; }
      std::cout<<segarecomp::emit_direct_flow_c(std::get<segarecomp::DirectFlowAnalysis>(discovered),state,*budget);
      return 0;
    }
    if (argc != 3) { print_usage(std::cerr); return 2; }
    const auto bytes = segarecomp::read_binary(argv[2]);
    const auto info = segarecomp::inspect_rom(bytes);
    if (command == "inspect") {
      std::cout << segarecomp::format_inspection(info);
      return info.outcome == segarecomp::ClassificationOutcome::recognized ? 0 : 1;
    }
    if (command == "analyze") {
      if (info.outcome != segarecomp::ClassificationOutcome::recognized || info.platform != segarecomp::Platform::genesis) {
        std::cerr << "segarecomp: " << segarecomp::diagnostic_name(info.diagnostic) << '\n';
        return 1;
      }
      const auto report = segarecomp::analyze_genesis_reset_image(bytes);
      std::cout << segarecomp::format_genesis_reset_image_report(report) << '\n';
      return report.outcome == segarecomp::ResetOutcome::accepted ? 0 : 2;
    }
    if (command == "emit-c") {
      if (info.outcome != segarecomp::ClassificationOutcome::recognized) {
        std::cerr << "segarecomp: " << segarecomp::diagnostic_name(info.diagnostic) << '\n';
        return 1;
      }
      std::cout << segarecomp::emit_c_manifest(info);
      return 0;
    }
    print_usage(std::cerr);
    return 2;
  } catch (const std::exception &error) {
    std::cerr << "segarecomp: " << error.what() << '\n';
    return 1;
  }
}

int main(int argc, char **argv) {
  if (argc >= 2 && std::string_view(argv[1]) == "build") return segarecomp_build_command(argc, argv);
  return run_cli(argc, argv);
}
