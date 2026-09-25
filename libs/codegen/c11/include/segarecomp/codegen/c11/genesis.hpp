#pragma once

#include <string>
#include <string_view>

namespace segarecomp {

// Strict-C11 bridge scaffolding. Machine analysis supplies only validated
// program facts; this owner renders the ABI-facing C source around them.
[[nodiscard]] std::string emit_genesis_runtime_c11_include();
// SEG-022-T003: the prelude split into the part every translation unit shares (includes) and the part only
// the main TU needs (report helpers). emit_genesis_bridge_c11_prelude is exactly
// "#define _POSIX_C_SOURCE 200809L\n" + shared_header_prelude + main_prelude.
[[nodiscard]] std::string emit_genesis_bridge_c11_shared_header_prelude();
[[nodiscard]] std::string emit_genesis_bridge_c11_main_prelude(std::string_view rom_sha256,
                                                                std::string_view cpu_dimensions);
[[nodiscard]] std::string emit_genesis_bridge_c11_prelude(std::string_view rom_sha256,
                                                           std::string_view cpu_dimensions);
[[nodiscard]] std::string emit_genesis_bridge_c11_main(std::string_view initial_ssp,
                                                        std::string_view entry_pc,
                                                        std::string_view dispatcher_name);
// SEG-007-T047 / ADR-0020 §6: when `irq6_handler_entry_hex` is non-empty the
// emitted `main` also sets `runtime.irq6_handler_entry` / `.irq6_handler_present`
// so the runtime IRQ6/VBlank admission mechanism has a build-resolved,
// never-runtime-fetched autovector target.
// SEG-007-T222 / ADR-0037: when `divide_by_zero_handler_entry_hex` is
// non-empty the emitted `main` also sets `runtime.divide_by_zero_handler_
// entry` / `.divide_by_zero_handler_present`, mirroring
// `irq6_handler_entry_hex`'s own wiring exactly for the vector-5 handler.
// SEG-021-T018 / ADR 0043: `privilege_violation_handler_entry_hex` wires the
// vector-8 handler the same way. The emitted `main` always establishes the
// MC68000 reset SR (S = 1, T = 0, I = 7).
[[nodiscard]] std::string emit_genesis_bridge_c11_main_open(std::string_view initial_ssp,
                                                             std::string_view entry_pc,
                                                             std::string_view irq6_handler_entry_hex = {},
                                                             std::string_view divide_by_zero_handler_entry_hex = {},
                                                             std::string_view privilege_violation_handler_entry_hex = {});
[[nodiscard]] std::string emit_genesis_bridge_c11_main_finish(std::string_view dispatcher_name);

}  // namespace segarecomp
