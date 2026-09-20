#include "segarecomp/c_emitter.hpp"
#include "segarecomp/direct_flow.hpp"
#include "segarecomp/moveq.hpp"
#include "segarecomp/codegen/c11/genesis_frontend.hpp"
#include "segarecomp/machine/genesis/frontend.hpp"
#include "segarecomp/rom.hpp"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#endif

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {
void print_usage(std::ostream &output) {
  output << "usage:\n  segarecomp inspect <image>\n  segarecomp analyze <image>\n  segarecomp emit-c <image>\n"
             "  segarecomp emit-moveq-c <image> <pc> <offset> <sr> <d0> <d1> <d2> <d3> <d4> <d5> <d6> <d7>\n"
               "  segarecomp emit-direct-flow-c <image> <pc> <offset> <sr> <budget> <d0> <d1> <d2> <d3> <d4> <d5> <d6> <d7>\n"
               "  segarecomp m68k-frontend <image> <source-id> <entry> <claim-name> <target-begin> <target-end> <image-begin> <image-end> [... ]\n"
               "  segarecomp emit-m68k-frontend-c <image> <source-id> <analysis-entry> <execution-entry> <sr> <budget> <d0> <d1> <d2> <d3> <d4> <d5> <d6> <d7> <claim-name> <target-begin> <target-end> <image-begin> <image-end> [... ]\n"
                "  segarecomp genesis-rom-startup <image>\n  segarecomp emit-genesis-rom-startup-c <image>\n"
                "  segarecomp genesis-general-startup <image>\n"
                  "  segarecomp emit-general-startup-bridge-c --rom <image> (--reset-entry [--analysis-seed <address-hex8>]... | --entry <address-hex8> --mapping-base <address-hex8>) --rom-sha256 <sha256> [--external-hints <path>] [--immutable-rom-aot]\n"
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

int main(int argc, char **argv) {
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
      for (int index = 4; index < argc;) {
        const std::string_view option = argv[index];
        if (option == "--reset-entry") {
          if (reset_entry) { print_usage(std::cerr); return 2; }
          reset_entry = true; ++index;
        } else if (option == "--immutable-rom-aot") {
          if (immutable_rom_aot) { print_usage(std::cerr); return 2; }
          immutable_rom_aot = true;
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
      }
      // ADR-0013 Decision §7 Phase B seed transport: an out-of-mapping seed
      // is not rejected here -- discover_m68k_general_startup's per-seed walk
      // already fails closed on an unmapped/conflicting instruction source
      // for any seed, exactly as it always has for the round-1 entry.
      for (const auto seed : analysis_seeds)
        program->runtime_confirmed_seeds.push_back({segarecomp::TargetAddressSpace::m68k_program, seed});
      const auto result = segarecomp::analyze_m68k_frontend(*program);
      if (immutable_rom_aot) {
        std::uint64_t aligned_start_count = 0U;
        for (const auto &range : program->immutable_rom_aot_ranges)
          aligned_start_count += (static_cast<std::uint64_t>(range.end_address) - range.begin_address + 1U) / 2U;
        std::uint64_t accepted_count = 0U;
        if (const auto *partial = std::get_if<segarecomp::FrontendPartialProgram>(&result))
          accepted_count = partial->accepted_prefix.immutable_rom_aot_entries.size();
        else if (const auto *accepted = std::get_if<segarecomp::FrontendAnalysis>(&result))
          accepted_count = accepted->immutable_rom_aot_entries.size();
        std::cerr << "segarecomp: immutable-rom AOT enumeration: aligned_start_count=" << aligned_start_count
                  << " accepted_count=" << accepted_count
                  << " rejected_count=" << (aligned_start_count - accepted_count) << '\n';
      }
      if (const auto *partial = std::get_if<segarecomp::FrontendPartialProgram>(&result)) {
        std::cout << segarecomp::emit_m68k_general_startup_bridge_c(*partial, digest_value);
        return 0;
      }
      if (const auto *accepted = std::get_if<segarecomp::FrontendAnalysis>(&result)) {
        std::cout << segarecomp::emit_m68k_general_startup_bridge_c(*accepted, digest_value);
        return 0;
      }
      std::cerr << segarecomp::format_m68k_frontend_result(result) << '\n';
      return 1;
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
