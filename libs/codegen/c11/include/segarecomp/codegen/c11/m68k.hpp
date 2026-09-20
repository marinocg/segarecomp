#pragma once

#include "segarecomp/core/provenance.hpp"
#include "segarecomp/cpu/m68k/direct_flow.hpp"
#include "segarecomp/cpu/m68k/instruction.hpp"
#include "segarecomp/cpu/m68k/ir.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace segarecomp {

// Minimal per-operand emission fact. The platform wrapper has already classified the operand
// (address-map classification is its own concern) and hands the lowering only how the
// access is to be emitted.
enum class M68kOperandAccess {
  resolved_constant,  // value already resolved at build time; emit the literal
  linear_memory,      // ordinary access to the caller-provided linear memory window
  runtime_routed      // access must go through the platform's runtime-routed access owner
};

// A lowered-flow unit as consumed by the structured direct-flow emitter.
struct M68kDirectFlowUnit { std::uint32_t ordinal{}; std::string id; std::vector<BlockId> members; BlockId entry_block{}; std::vector<InstructionProvenance> provenance; };

struct M68kMemoryEmissionContext;

// Narrow platform seam: the platform wrapper owns the C text of its runtime-routed access
// protocol (access, stop and provenance ABI). M68k lowering decides WHEN a runtime access, stack
// guard, call push, divide trap or exception return is needed and owns all CPU semantics
// (address masking, stack-pointer and PC updates); the emitter supplies only the platform text.
class M68kRuntimeCEmitter {
 public:
  virtual ~M68kRuntimeCEmitter() = default;
  // C expression naming the current instruction's provenance (assigned to runtime_source).
  [[nodiscard]] virtual std::string instruction_source(const M68kIrOperation &operation) const = 0;
  [[nodiscard]] virtual std::string_view default_provenance_helper() const = 0;
  // Statement text following `const uint32_t <routed_addr> = ...;`. Expression is `value`.
  [[nodiscard]] virtual std::string routed_read(const M68kMemoryEmissionContext &context, std::string_view routed_addr,
                                                std::string_view value, std::string_view stop,
                                                M68kMemoryAccessWidth size) const = 0;
  // Text following `{ const uint32_t <routed_addr> = ...; `, ending before the lowering's closing `}`.
  [[nodiscard]] virtual std::string routed_write(const M68kMemoryEmissionContext &context, std::string_view routed_addr,
                                                 std::string_view stop, M68kMemoryAccessWidth size,
                                                 std::string_view value) const = 0;
  // Guarded pop of the return address; must leave it in `m68k_observed_return`. The lowering
  // then advances the stack pointer and assigns the program counter.
  [[nodiscard]] virtual std::string return_pop_guard(const M68kMemoryEmissionContext &context, std::string_view stack_pointer) const = 0;
  // Statement `return ...;` taken when an indirect control target is not a member of the target set.
  [[nodiscard]] virtual std::string unresolved_indirect_stop(const M68kMemoryEmissionContext &context) const = 0;
  // Guarded push of `m68k_continuation` at stack_pointer-4 into `m68k_new_a7`; the lowering then
  // commits the stack pointer and program counter. `expanded` selects the multi-line layout.
  [[nodiscard]] virtual std::string call_push_guard(const M68kMemoryEmissionContext &context, std::string_view stack_pointer,
                                                    bool expanded) const = 0;
  // Divide-by-zero trap: continues at the handler or stops. Text ends before the lowering's `else { `.
  [[nodiscard]] virtual std::string divide_by_zero(const M68kMemoryEmissionContext &context, std::uint32_t next_pc) const = 0;
  // Exception return: leaves the restored program counter in `m68k_rte_pc`.
  [[nodiscard]] virtual std::string exception_return(const M68kMemoryEmissionContext &context) const = 0;
};

struct M68kMemoryEmissionContext {
  std::string_view ram_array;
  std::string_view address_registers;
  std::string_view user_stack_pointer;
  std::string_view frame_ids_array;
  std::string_view frame_continuations_array;
  std::string_view frame_depth;
  std::uint32_t continuation{};
  std::optional<M68kOperandAccess> test_operand_access;
  std::uint32_t test_operand_value{};
  std::vector<M68kOperandAccess> movem_transfer_access;
  std::vector<std::uint32_t> movem_transfer_values;
  // When present for routed (An)/(An)+ MOVEM reads, the immutable values
  // above are valid only if the instruction-boundary architectural base
  // still equals this independently validated producer result. A different
  // live base selects the ordinary routed reads in the same emitted body.
  std::optional<std::uint32_t> movem_transfer_fold_base;
  // Caller-owned linear-memory window [begin, end) addressed by `ram_array`, and the platform's
  // runtime-routed C text owner (required whenever runtime_routing is set).
  std::uint32_t linear_memory_begin{};
  std::uint32_t linear_memory_end{};
  const M68kRuntimeCEmitter *runtime_emitter{};
  std::string_view program_counter{"pc"};
  // Optional caller-owned timing source slot.  MUL lowering writes the word
  // it already materialized after a successful routed read.
  std::string_view timing_mul_source;
  // Optional caller-owned DBcc taken-branch timing output. The DBcc lowerer
  // assigns this byte only on its taken branch; callers that retire the
  // instruction supply a zero-initialized local for the fallthrough cases.
  std::string_view timing_dbcc_taken;
  // SEG-007-T252 / ADR-0040: the former SEG-007-T107 `loop_progress_object` /
  // `finite_loop_progress_proof`, SEG-007-T155/ADR-0017 `data_progress_proof`,
  // and SEG-007-T157/ADR-0019 `read_data_progress_proof` / `data_progress_
  // read_region` fields fed only the now-removed generated-runtime progress
  // watchdog's guarded note emission. No emitter path reads or writes them
  // any longer, so they have been removed from this context entirely.
  bool runtime_routing{};
  // SEG-020-T003: generation-time diagnostics option; stack accesses then carry their bus kind.
  bool execution_history_hooks{};
  std::string_view runtime_object;
  std::string_view runtime_source;
  std::string_view runtime_provenance_helper;
  std::vector<std::uint32_t> runtime_return_targets;
  // SEG-007-T124 / ADR-0009: the proven, sorted, deduplicated candidate
  // target set for a computed/indirect control transfer (currently JSR
  // through a brief PC-relative indexed EA), populated only by the C4
  // caller that already independently validated this exact source
  // instruction's retained `M68kIndirectTargetEaSet`. Empty for every other
  // operation.
  std::vector<std::uint32_t> indirect_candidate_targets;
  // SEG-007-T174: true only for the ONE caller (frontend.cpp's shared
  // subtract/add/logical C4 case) that wraps its `emit_m68k_operation_c`
  // call with a `#define pc runtime->pc` / `#undef pc` text bridge. That
  // caller needs the wrapped body to emit the bare `pc` identifier (so the
  // macro performs exactly one substitution); SUBQ/SUBI's sibling caller
  // (frontend.cpp, same shared subtract-family m68k.cpp emitter, no bridge)
  // needs the fully-qualified `program_counter` text instead. Both callers
  // always set `program_counter` to the same "runtime->pc" text, so that
  // field alone cannot distinguish them -- this flag is the disambiguator.
  // Defaults false (fully-qualified text), matching every caller except the
  // one bridge-wrapped call site.
  bool pc_macro_bridge_active{};
  M68kMemoryEmissionContext() = default;
  M68kMemoryEmissionContext(std::string_view ram, std::string_view address, std::string_view frame_ids,
                            std::string_view frame_continuations, std::string_view depth,
                            std::uint32_t static_continuation,
                            std::optional<M68kOperandAccess> operand_access,
                            std::uint32_t operand_value,
                            std::vector<M68kOperandAccess> transfer_access = {},
                            std::vector<std::uint32_t> transfer_values = {})
      : ram_array(ram), address_registers(address), frame_ids_array(frame_ids),
        frame_continuations_array(frame_continuations), frame_depth(depth), continuation(static_continuation),
        test_operand_access(operand_access), test_operand_value(operand_value),
        movem_transfer_access(std::move(transfer_access)), movem_transfer_values(std::move(transfer_values)) {}
};

[[nodiscard]] std::string emit_m68k_operation_c(const M68kIrOperation &operation, std::string_view data_registers, std::string_view status_register, std::string_view indent = {}, const M68kMemoryEmissionContext *memory = nullptr);
// Nonsemantic compatibility-probe assessment.  Codegen owns the synthetic
// context needed to inspect complete lowering, so callers cannot mistake a
// context-free fragment for supported generated execution.
[[nodiscard]] bool m68k_operation_has_complete_c_emission(const M68kIrOperation &operation);
[[nodiscard]] std::string emit_m68k_direct_flow_c(const DirectFlowAnalysis &analysis, const DirectFlowState &initial, std::uint64_t budget);
[[nodiscard]] std::string emit_m68k_structured_direct_flow_c(const DirectFlowAnalysis &analysis, std::span<const M68kDirectFlowUnit> units, const DirectFlowState &initial, std::uint64_t budget);
} // namespace segarecomp
