#include "segarecomp/codegen/c11/genesis.hpp"

#include <string>

namespace segarecomp {

std::string emit_genesis_runtime_c11_include() {
  return "#include \"runtime.h\"\n\n";
}

std::string emit_genesis_bridge_c11_shared_header_prelude() {
  return emit_genesis_runtime_c11_include() +
         "#if defined(_WIN32)\n#include <fcntl.h>\n#include <io.h>\n#endif\n#include <errno.h>\n#include <limits.h>\n#include <stdlib.h>\n#include <string.h>\n\n";
}

std::string emit_genesis_bridge_c11_prelude(std::string_view rom_sha256,
                                            std::string_view cpu_dimensions) {
  return "#define _POSIX_C_SOURCE 200809L\n" + emit_genesis_bridge_c11_shared_header_prelude() +
         emit_genesis_bridge_c11_main_prelude(rom_sha256, cpu_dimensions);
}

std::string emit_genesis_bridge_c11_main_prelude(std::string_view rom_sha256,
                                                 std::string_view cpu_dimensions) {
  return std::string() +
         "static const char GENESIS_BRIDGE_ROM_SHA256[65] = \"" + std::string(rom_sha256) +
         "\";\nstatic const GenesisReportMetadata GENESIS_BRIDGE_REPORT_METADATA = { " +
         std::string(cpu_dimensions) +
         " };\n"
         // SEG-007-T252 / ADR-0040: this helper is now handed already-parsed
         // path/fd strings (never argc/argv directly) so argv validation can
         // happen once, up front, before genesis_runtime_run is ever called.
         // SEG-018-T006: the one native inherited-stream opener. The historical
         // `-fd` argv spelling carries the inherited native writer token: a POSIX
         // descriptor number, or on Windows an inherited Win32 HANDLE value that is
         // converted to a CRT fd HERE, in the child (a parent CRT fd number is not a
         // valid identity in this process). fclose() stays the flush/close boundary.
         "static FILE *genesis_open_inherited_report_stream(const char *token) { char *end; unsigned long long value; if (token == NULL || *token < '0' || *token > '9') return NULL; errno = 0; value = strtoull(token, &end, 10); if (errno != 0 || *end != '\\0') return NULL;\n"
         "#if defined(_WIN32)\n"
         "  { int crt_fd; FILE *stream; if (value > (unsigned long long)UINTPTR_MAX) return NULL; crt_fd = _open_osfhandle((intptr_t)(uintptr_t)value, _O_WRONLY | _O_BINARY); if (crt_fd < 0) return NULL; stream = _fdopen(crt_fd, \"wb\"); if (stream == NULL) _close(crt_fd); return stream; }\n"
         "#else\n"
         "  if (value > (unsigned long long)INT_MAX) { return NULL; }\n  return fdopen((int)value, \"w\");\n"
         "#endif\n"
         "}\n"
         "static int genesis_write_requested_full_report(const char *report_path, const char *report_fd, const GenesisRuntime *runtime, const GenesisControlTransfer *result) { FILE *full; if (report_path == NULL && report_fd == NULL) return 0; if (report_path != NULL) full = fopen(report_path, \"w\"); else full = genesis_open_inherited_report_stream(report_fd); if (full == NULL) return 1; if (genesis_write_full_report(full, runtime, result, GENESIS_BRIDGE_ROM_SHA256, &GENESIS_BRIDGE_REPORT_METADATA) != 0 || fclose(full) != 0) return 1; return 0; }\n"
         // SEG-007-T252 / ADR-0040 correction: sibling helper for the
         // dedicated, FD-only ephemeral PC-history diagnostic channel. This
         // is an INDEPENDENT transport from the stable full-report writer
         // above -- no filesystem-path form exists for it -- and it is a
         // no-op when the caller did not request it (normal generated
         // execution never touches this channel).
         "static int genesis_write_requested_ephemeral_pc_history(const char *ephemeral_report_fd, const GenesisRuntime *runtime, const GenesisControlTransfer *result) { FILE *ephemeral; if (ephemeral_report_fd == NULL) return 0; ephemeral = genesis_open_inherited_report_stream(ephemeral_report_fd); if (ephemeral == NULL) return 1; if (genesis_write_ephemeral_pc_history(ephemeral, runtime, result) != 0 || fclose(ephemeral) != 0) return 1; return 0; }\n"
         // SEG-007-T252 / ADR-0040: order-independent argv pair scanner.
         // argc must be 1 (no options), 3 (one recognised pair), 5 (two
         // distinct recognised pairs), or 7 (three distinct recognised
         // pairs); anything else -- an unrecognised flag, a duplicate/
         // conflicting flag, or a malformed --instruction-budget value
         // (empty, non-digit, embedded NUL via strtoul's own '\\0' check,
         // leading '+'/'-', a value exceeding UINT32_MAX, or a value of
         // exactly zero) -- fails closed with a nonzero return before any
         // runtime state is touched and before genesis_runtime_run is ever
         // called, so a bad *runner* option can never surface as a guest
         // semantic stop (e.g. genesis_internal_dispatch_inconsistency_stop).
         // `*instruction_budget_out` is left at the caller's compiled-in
         // runner default when no `--instruction-budget` pair is present; an
         // internal duplicate-flag guard (not exposed to the caller) rejects
         // a repeated `--instruction-budget` pair. `--ephemeral-report-fd` is
         // an independent diagnostic channel with its own duplicate guard but
         // NO conflict rule against `--full-report-path`/`--full-report-fd`.
         "static int genesis_parse_bridge_argv(int argc, char **argv, const char **report_path_out, const char **report_fd_out, uint32_t *instruction_budget_out, const char **ephemeral_report_fd_out) { int index; int instruction_budget_seen = 0; *report_path_out = NULL; *report_fd_out = NULL; *ephemeral_report_fd_out = NULL; if (argc == 1) return 0; if (argc != 3 && argc != 5 && argc != 7) return 1; for (index = 1; index < argc; index += 2) { const char *flag = argv[index]; const char *value = argv[index + 1]; if (strcmp(flag, \"--full-report-path\") == 0) { if (*report_path_out != NULL || *report_fd_out != NULL) return 1; *report_path_out = value; } else if (strcmp(flag, \"--full-report-fd\") == 0) { if (*report_path_out != NULL || *report_fd_out != NULL) return 1; *report_fd_out = value; } else if (strcmp(flag, \"--ephemeral-report-fd\") == 0) { if (*ephemeral_report_fd_out != NULL) return 1; *ephemeral_report_fd_out = value; } else if (strcmp(flag, \"--instruction-budget\") == 0) { char *end; unsigned long parsed; size_t k; if (instruction_budget_seen) return 1; if (value[0] == '\\0') return 1; for (k = 0; value[k] != '\\0'; ++k) { if (value[k] < '0' || value[k] > '9') return 1; } errno = 0; parsed = strtoul(value, &end, 10); if (errno != 0 || *end != '\\0' || parsed > UINT32_MAX || parsed == 0) return 1; *instruction_budget_out = (uint32_t)parsed; instruction_budget_seen = 1; } else return 1; } return 0; }\n";
}

std::string emit_genesis_bridge_c11_main(std::string_view initial_ssp, std::string_view entry_pc,
                                         std::string_view dispatcher_name) {
  return emit_genesis_bridge_c11_main_open(initial_ssp, entry_pc) +
         emit_genesis_bridge_c11_main_finish(dispatcher_name);
}

std::string emit_genesis_bridge_c11_main_open(std::string_view initial_ssp, std::string_view entry_pc,
                                             std::string_view irq6_handler_entry_hex,
                                             std::string_view divide_by_zero_handler_entry_hex,
                                             std::string_view privilege_violation_handler_entry_hex,
                                             const std::vector<std::pair<std::uint32_t, std::string>>
                                                 &software_exception_handler_entry_hex) {
  std::string open =
      "int main(int argc, char **argv) { GenesisRuntime runtime = {0}; GenesisControlTransfer result; "
      "const char *report_path; const char *report_fd; uint32_t instruction_budget = UINT32_C(128); "
      "const char *ephemeral_report_fd; "
      "if (genesis_parse_bridge_argv(argc, argv, &report_path, &report_fd, &instruction_budget, &ephemeral_report_fd) != 0) return 1; "
      "runtime.a[7] = UINT32_C(" +
      std::string(initial_ssp) + "); runtime.pc = UINT32_C(" + std::string(entry_pc) +
      "); runtime.sr = UINT16_C(0x2700); ";  // SEG-021-T018 / ADR 0043 §6: reset state S = 1, T = 0, I = 7
  if (!irq6_handler_entry_hex.empty())
    open += "runtime.irq6_handler_entry = UINT32_C(" + std::string(irq6_handler_entry_hex) +
            "); runtime.irq6_handler_present = 1; ";
  if (!divide_by_zero_handler_entry_hex.empty())
    open += "runtime.divide_by_zero_handler_entry = UINT32_C(" + std::string(divide_by_zero_handler_entry_hex) +
            "); runtime.divide_by_zero_handler_present = 1; ";
  if (!privilege_violation_handler_entry_hex.empty())
    open += "runtime.privilege_violation_handler_entry = UINT32_C(" + std::string(privilege_violation_handler_entry_hex) +
            "); runtime.privilege_violation_handler_present = 1; ";
  // SEG-021-T019 / ADR 0043 §7: the software-exception vector table, indexed by vector number.
  for (const auto &[vector, handler_hex] : software_exception_handler_entry_hex)
    open += "runtime.software_exception_handler_entry[" + std::to_string(vector) + "] = UINT32_C(" + handler_hex +
            "); runtime.software_exception_handler_present[" + std::to_string(vector) + "] = 1; ";
  return open;
}

std::string emit_genesis_bridge_c11_main_finish(std::string_view dispatcher_name) {
  // SEG-007-T252 / ADR-0040: `instruction_budget` (parsed above, defaulting to
  // 128 when no `--instruction-budget` argv pair is present) is now a RUNNER
  // allowance passed to genesis_runtime_run, not a guest-semantic no-progress
  // window -- there is no watchdog/progress-credit concept left to describe.
  // 128 is retained ONLY as the compiled-in default for zero-argument argv
  // compatibility (same argv shape, same generated-binary default when no
  // `--instruction-budget` flag is passed) -- it is NOT a claim that a
  // zero-argument invocation executes an equivalent depth of guest dispatch
  // to the former watchdog window. The former semantic-watchdog `W = 128` was
  // a *consecutive no-progress* window that legitimate recurring progress
  // kept resetting, so real execution ran for millions of dispatches; this
  // `128` is a *total* dispatch count, a radically shorter bound. Any
  // automated/headless caller that wants a depth comparable to historical
  // watchdog-bounded runs must pass an explicit `--instruction-budget`
  // (see `GENESIS_CANONICAL_RUNNER_DISPATCH_ALLOWANCE` in
  // tools/genesis_startup_bridge.py -- the runner/tooling layer, not this
  // generated runtime -- which the canonical/headless
  // `tools/genesis_startup_bridge.py` CLI path uses by default instead of
  // silently falling through to this lower-level, unrelated 128 default).
  // This value carries no hardware/timing meaning and callers may override
  // it explicitly.
  return "result = genesis_runtime_run(&runtime, " + std::string(dispatcher_name) +
          ", instruction_budget); if (genesis_write_requested_full_report(report_path, report_fd, &runtime, &result) != 0) return 1; if (genesis_write_requested_ephemeral_pc_history(ephemeral_report_fd, &runtime, &result) != 0) return 1; return genesis_write_sanitized_report(&result, GENESIS_BRIDGE_ROM_SHA256, &GENESIS_BRIDGE_REPORT_METADATA); }\n";
}

}  // namespace segarecomp
