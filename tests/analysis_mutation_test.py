#!/usr/bin/env python3
"""SEG-029-T005 (ADR 0078): adversarial mutation gate for the generic abstract-analysis core and its CPU adapters.

Every mutant below is one precise textual edit of PRODUCTION analysis code (the generic core headers, the M68K adapter, the Z80
effect projection or the Z80 adapter) that plants a deliberate soundness, determinism or bound defect. The harness copies the
product tree (without build/, .git/, games/, .tools/) to a temporary directory, configures ONE build there, builds the
SEG-029 fixture tests plus the SEG-030-T003 address-domain, SEG-030-T004 memory, SEG-030-T005 contexts, SEG-030-T006 frames and Genesis
interrupt-premise and the SEG-030-T010 Z80 store-freedom proof fixtures (SEG-030-T008 maps every SEG-030 mutant of its record to a
killed mutant here; ADR 0079, T008 record; T008 part 2 adds the return-slot fixture) and requires the unmutated baseline to pass. Then, per mutant, it applies the edit to the temporary copy only, rebuilds the
fixture tests incrementally, runs the affected ones, and restores the file (SourceStamper: strictly increasing source stamps plus a
settled build tree, so neither a mutation nor a restoration is ever skipped by a whole-second build tool; an executable that is not
relinked after a mutation fails the harness as a stale build). A mutant is KILLED when one of its fixture tests exits non-zero (or
times out). The worktree is never modified.

Fail-closed rules:
  * stale mutant: the edit's `old` text must occur exactly once in the current source, otherwise the harness FAILS (a refactor can
    never silently disable a mutant; update the mutant together with the refactor);
  * a mutant that does not compile FAILS the harness (compilation failure is not a kill);
  * a surviving mutant FAILS the harness, unless it is listed as `equivalent` with a written justification, in which case it must
    still compile and must SURVIVE (a killed "equivalent" mutant means the justification is stale and FAILS the harness).
Self-checks: the baseline must pass all fixture tests, and the stale-edit detector must reject a missing and a duplicated pattern.

Equivalent mutants (justified, asserted to survive):
  * m68k_failed_read_ignored: `m68k_finite_register_after` (the CPU semantic owner) already returns a default, not-`known`
    `M68kFiniteValues` (Unknown) whenever `immutable_read_failed` is set, so the adapter's explicit override is defence in depth.
  * m68k_flag_setter_adjacency_unchecked: a state's `flag_setter` is only ever created on a fallthrough/branch edge whose target is
    the physically next instruction (`target == next`), joins keep only agreeing setters, and entries/continuations carry none, so
    the re-check of physical adjacency at the consumer can never fail.

SEG-031 (ADR 0080) adds the `hybrid` group (`--group hybrid`, its own CTest `analysis_hybrid_mutation_test`): mutants of the hybrid
admission planner, the CPU-owned island edges and the production validate-and-filter seam, judged by the cheap hybrid fixtures only
(`analysis_hybrid_plan_test`, `genesis_hybrid_admission_test`). The default group is unchanged.

usage: analysis_mutation_test.py <product-root> <cmake> <c-compiler> <cxx-compiler> [--generator G] [--make-program P]
                                 [--group analysis|hybrid] [--only NAME[,NAME...]] [--keep]
"""
from __future__ import annotations

import argparse
import dataclasses
import io
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

CORE_TEST = "analysis_core_test"
M68K_TEST = "analysis_m68k_equivalence_test"
Z80_TEST = "analysis_z80_adapter_test"
CONTEXTS_TEST = "analysis_m68k_contexts_test"  # SEG-030-T005: call contexts and callee summaries
FRAMES_TEST = "analysis_m68k_frames_test"  # SEG-030-T006: interrupt mask, handler instances, frames and returns
PREMISE_TEST = "analysis_genesis_interrupt_premise_test"  # SEG-030-T006: the named Genesis interrupt-source premise
Z80_PROOF_TEST = "analysis_genesis_z80_proof_test"  # SEG-030-T010: the Genesis Z80 work-RAM store-freedom proof and its credit
VALUE_TEST = "analysis_m68k_value_test"  # SEG-030-T003: the address region plus offset domain (added by SEG-030-T008)
MEMORY_TEST = "analysis_m68k_memory_test"  # SEG-030-T004: abstract memory and alias exclusion (added by SEG-030-T008)
RETURN_SLOT_TEST = "analysis_m68k_return_slot_test"  # SEG-030-T008: the return slot and the return-slot integrity premise
PRECISION_TEST = "analysis_m68k_precision_test"  # SEG-034: the whole-image-trigger precision refinements (ADR 0081)
# cheapest first: a core mutant is usually decided by the CPU-free fixture
ALL_TESTS = (CORE_TEST, Z80_TEST, M68K_TEST, VALUE_TEST, MEMORY_TEST, RETURN_SLOT_TEST, PRECISION_TEST, CONTEXTS_TEST, FRAMES_TEST,
             PREMISE_TEST, Z80_PROOF_TEST)
# Per-fixture time limit: a mutant that makes a fixture loop is killed by it, and a baseline that exceeds it fails the harness (a timeout
# is never a pass). The SEG-029/SEG-030 fixtures run unmutated in well under a second and keep 60 s.
TEST_TIMEOUT_SECONDS = 60

FINITE = "libs/analysis/include/segarecomp/analysis/finite_value.hpp"
SOLVER = "libs/analysis/include/segarecomp/analysis/solver.hpp"
M68K = "libs/cpu/m68k/analysis/src/finite_adapter.cpp"
M68K_FRAMES = "libs/cpu/m68k/analysis/src/frames.cpp"
M68K_ADDRESS = "libs/cpu/m68k/analysis/src/address_value.cpp"
M68K_MEMORY = "libs/cpu/m68k/analysis/src/abstract_memory.cpp"
M68K_ADAPTER_HEADER = "libs/cpu/m68k/analysis/include/segarecomp/cpu/m68k/analysis/finite_adapter.hpp"
Z80_EFFECTS = "libs/cpu/z80/src/effects.cpp"
Z80_ADAPTER = "libs/cpu/z80/analysis/src/adapter.cpp"
GENESIS_PREMISE = "platforms/genesis/analysis_report/include/segarecomp/genesis_analysis_report/interrupt_premise.hpp"
Z80_PROOF = "platforms/genesis/analysis_report/src/z80_ram_write_proof.cpp"
REPORT = "platforms/genesis/analysis_report/src/report.cpp"
# SEG-031 (ADR 0080): the hybrid admission gate (`--group hybrid`): its own cheap fixture set.
HYBRID_PLAN_TEST = "analysis_hybrid_plan_test"  # the report-only planner on synthetic adversarial shapes
ADMISSION_TEST = "genesis_hybrid_admission_test"  # the production plan parser and fail-closed validate-and-filter seam
HYBRID_TESTS = (ADMISSION_TEST, HYBRID_PLAN_TEST)
GROUP_TESTS = {"analysis": ALL_TESTS, "hybrid": HYBRID_TESTS}
# SEG-031: the hybrid planner fixture plans every adversarial shape through the full SEG-030 instantiation in an unoptimized build
# (about 20 s on a developer host, 97 s observed for the unmutated fixture beside a concurrent mutation build on a 4-core Linux CI
# runner, where 60 s timed the baseline out). It gets an explicit limit with headroom over that contended observation (still finite, so
# a non-terminating mutant is killed), and `--fail-fast` (exit at the first failed check) so that a killed mutant stops early; an
# unmutated baseline always runs every check.
FIXTURE_TIMEOUT_SECONDS = {HYBRID_PLAN_TEST: 600}
FIXTURE_ARGUMENTS = {HYBRID_PLAN_TEST: ("--fail-fast",)}
HYBRID_PLAN = "platforms/genesis/analysis_report/src/hybrid_plan.cpp"
ADMISSION = "platforms/genesis/machine/src/hybrid_admission.cpp"


@dataclasses.dataclass(frozen=True)
class Mutant:
    name: str
    path: str
    old: str
    new: str
    tests: tuple[str, ...]
    summary: str
    equivalent: str = ""  # non-empty: written justification; the mutant must compile and survive
    group: str = "analysis"  # "analysis" (SEG-029/SEG-030, the default run) or "hybrid" (SEG-031, `--group hybrid`)


class StaleMutant(Exception):
    pass


MUTANTS: list[Mutant] = [
    # ---------------------------------------------------------------- generic core (header-only: every consumer is rebuilt)
    Mutant("dropped_join", SOLVER,
           "    found->second = join(found->second, state);",
           "    found->second = state;",
           ALL_TESTS, "solver overwrites a reached point's state instead of joining"),
    Mutant("dropped_join_value", FINITE,
           "    return of(std::move(merged), bound);\n  }\n\n  // Partial order",
           "    return of(std::vector<std::uint64_t>(left.values_), bound);\n  }\n\n  // Partial order",
           ALL_TESTS, "join of two precise sets returns the left side"),
    Mutant("set_bound_ignored", FINITE,
           "    if (values.size() > std::min(bound, default_set_bound)) return unknown(UnknownReason::set_bound);\n",
           "    (void)bound;\n",
           ALL_TESTS, "of()/join never yields Unknown(set_bound)"),
    Mutant("unknown_swallowed", FINITE,
           "      if (!left.is_unknown()) return right;",
           "      if (!left.is_unknown()) return left;",
           ALL_TESTS, "a precise set joined with Unknown yields the set"),
    Mutant("unknown_as_bottom", FINITE,
           "    if (left.is_bottom()) return right;\n    if (right.is_bottom()) return left;\n",
           "    if (left.is_bottom() || left.is_unknown()) return right;\n"
           "    if (right.is_bottom() || right.is_unknown()) return left;\n",
           ALL_TESTS, "Unknown is treated as the join identity (bottom)"),
    Mutant("leq_too_permissive", FINITE,
           "    return std::includes(right.values_.begin(), right.values_.end(), left.values_.begin(), left.values_.end());",
           "    return true;",
           ALL_TESTS, "leq(set, set) is always true: premature fixed point"),
    Mutant("premature_fixed_point", SOLVER,
           "    if (leq(state, found->second)) return true;",
           "    if (leq(state, found->second) || true) return true;",
           ALL_TESTS, "solver treats every re-arrival at a reached point as no change"),
    Mutant("nondeterministic_order", SOLVER,
           "      const auto point = *worklist.begin();\n      worklist.erase(worklist.begin());",
           "      const auto point = *worklist.rbegin();\n      worklist.erase(std::prev(worklist.end()));",
           ALL_TESTS, "worklist pops the largest pending point instead of the smallest"),
    Mutant("iteration_bound_ignored", SOLVER,
           "    if (out.iterations >= bounds.max_iterations) {",
           "    if (false) {",
           ALL_TESTS, "the iteration bound is never enforced"),
    Mutant("state_bound_ignored", SOLVER,
           "      if (out.in_states.size() >= bounds.max_points) return false;\n",
           "",
           ALL_TESTS, "the program-point bound is never enforced"),
    Mutant("incomplete_answers_partial", SOLVER,
           "    if (!complete) return FiniteValue::unknown(reason);\n",
           "",
           ALL_TESTS, "query returns the partial state of an incomplete solve"),
    Mutant("incomplete_reached_partial", SOLVER,
           "return complete && in_states.contains(point);",
           "return in_states.contains(point);",
           ALL_TESTS, "reached() answers from an incomplete solve"),
    Mutant("stale_site_resolution", SOLVER,
           "      out.unresolved_computed.erase(point);\n      out.computed_targets.erase(point);\n",
           "",
           ALL_TESTS, "a computed site keeps an earlier transfer's resolution"),
    Mutant("stale_target_not_pinned", SOLVER,
           "      restart = true;\n",
           "",
           ALL_TESTS, "a site that lost an emitted computed target is not pinned (stale precise state at its old target)"),
    Mutant("pinned_site_keeps_edges", SOLVER,
           "          if (pin != pinned.end()) continue;  // a pinned site contributes no computed edge\n",
           "",
           ALL_TESTS, "a pinned site still emits computed edges after the restart"),
    Mutant("bounds_raised", SOLVER,
           "  const Bounds bounds{std::min(requested.max_iterations, default_max_iterations),\n",
           "  const Bounds bounds{requested.max_iterations,\n",
           (CORE_TEST,), "a caller-requested iteration bound above the default is honoured"),
    # ---------------------------------------------------------------- M68K adapter
    Mutant("m68k_flag_setter_join_kept", M68K,
           "  out.flag_setter = left.flag_setter == right.flag_setter ? left.flag_setter : std::nullopt;",
           "  out.flag_setter = left.flag_setter ? left.flag_setter : right.flag_setter;",
           (M68K_TEST,), "branch filter provenance survives a join with another predecessor"),
    Mutant("m68k_flag_leq_permissive", M68K,
           "  return !right.flag_setter || left.flag_setter == right.flag_setter;",
           "  return true;",
           (M68K_TEST,), "leq ignores the flag-setter provenance (a second predecessor never removes it)"),
    Mutant("m68k_width_join_dropped", M68K,
           "(left.width_derived[i] || right.width_derived[i])",
           "(left.width_derived[i] && right.width_derived[i])",
           (M68K_TEST,), "join drops the width-only annotation unless both sides carry it"),
    Mutant("m68k_width_only_admitted", M68K,
           "  if (in.width_derived[slot] && !config_.accept_width_domains) return finish(M68kPcIndexOutcome::width_only_domain);",
           "  (void)config_.accept_width_domains;",
           (M68K_TEST,), "the strict policy admits width-only index domains"),
    Mutant("m68k_solver_pin_ignored", M68K,
           "      if (out.solution.pinned.contains(point) && !config.pinned_sites.contains(pc)) invalidated.insert(pc);\n",
           "",
           (M68K_TEST,), "a site pinned by the generic solver is reported from its narrower final input"),
    Mutant("m68k_store_derived_invalidation_skipped", M68K,
           "      if (out.solution.pinned.contains(point) && !config.pinned_sites.contains(pc)) invalidated.insert(pc);\n",
           "",
           (MEMORY_TEST,), "skipped store-derived invalidation: a field-dispatch proof undone by the store it exposed is not reported "
           "invalidated (the same edit as m68k_solver_pin_ignored, decided by the SEG-030-T004 store-derived fixture alone)"),
    Mutant("m68k_odd_target_kept", M68K,
           "    if ((target & 1U) != 0U) {\n      ++out.odd_targets_excluded;  // an odd JMP/JSR target",
           "    if ((target & 1U) != 0U && false) {\n      ++out.odd_targets_excluded;  // an odd JMP/JSR target",
           (M68K_TEST,), "odd JMP/JSR targets are not excluded"),
    Mutant("m68k_target_outside_partial", M68K,
           "    if (!image_.mapped(target)) return finish(M68kPcIndexOutcome::target_outside_image);",
           "    if (!image_.mapped(target)) continue;",
           (M68K_TEST,), "a target outside the image is dropped, leaving a partial set"),
    Mutant("m68k_failed_read_reason_lost", M68K,
           "      const auto reason = transfer.immutable_read_failed ? UnknownReason::non_immutable_read\n",
           "      const auto reason = transfer.immutable_read_failed ? UnknownReason::unsupported_transfer\n",
           (M68K_TEST,), "a failed immutable read loses its typed reason"),
    Mutant("m68k_failed_read_ignored", M68K,
           "transfer.immutable_read_failed ? M68kFiniteValues::unknown() : transfer.values,",
           "transfer.values,",
           (M68K_TEST,), "the adapter does not force a failed immutable read to Unknown",
           equivalent="the CPU owner already returns an Unknown value whenever immutable_read_failed is set"),
    Mutant("m68k_flag_setter_adjacency_unchecked", M68K,
           "    if (setter && ((*in.flag_setter + setter->length) & bus_mask) != pc) setter.reset();\n",
           "",
           (M68K_TEST,), "the consumer does not re-check that the flag setter is physically adjacent",
           equivalent="flag_setter is only created on an edge to the physically next instruction"),
    Mutant("m68k_unresolved_unreported", M68K,
           "        result.unresolved_computed = report.reason;\n",
           "",
           (M68K_TEST,), "an unresolved PC-indexed site is not reported unresolved"),
    Mutant("m68k_call_continuation_keeps_state", M68K,
           "if (config_.call_continuations) result.edges.push_back({stacked, EdgeKind::return_edge, entry_state(true, tag)});",
           "if (config_.call_continuations) result.edges.push_back({stacked, EdgeKind::return_edge, out});",
           (M68K_TEST,), "a call continuation carries the pre-call state instead of an opaque entry"),
    # ---------------------------------------------------------------- M68K address domain and abstract memory (SEG-030-T003/T004;
    # added by the SEG-030-T008 soundness gate)
    Mutant("m68k_offset_region_exit_accepted", M68K_ADDRESS,
           "  if (new_lo < 0 || new_hi > static_cast<std::int64_t>(limit)) return std::nullopt;  // leaves the region: never clamped",
           "  if (new_lo < 0 && limit == 0U) return std::nullopt;",
           (VALUE_TEST,), "out-of-region offset accepted: address arithmetic leaving the region extent is not region_exit"),
    Mutant("m68k_congruence_as_exact", M68K_ADDRESS,
           "  return is_known() && std::none_of(pairs.begin(), pairs.end(), [](const auto &pair) { return pair.second.is_strided(); });",
           "  return is_known();",
           (VALUE_TEST,), "congruence widened to certainty: a strided (congruence-only) points-to set counts as an exact set"),
    Mutant("m68k_memory_weak_update_dropped", M68K_MEMORY,
           "                      if (!strong && storable && it->first == cell) {\n"
           "                        auto joined = m68k_cell_join(it->second, *value, span);",
           "                      if (!strong && storable && it->first == cell) {\n"
           "                        auto joined = std::optional<M68kCellValue>(it->second);",
           (MEMORY_TEST,), "dropped weak update: a may-store keeps the old cell value instead of joining the stored value"),
    Mutant("m68k_memory_unknown_base_not_poisoned", M68K_MEMORY,
           "  if (!targets.is_known()) {\n    memory.poison_all();\n    return;\n  }",
           "  if (!targets.is_known()) {\n    return;\n  }",
           (MEMORY_TEST,), "ignored unknown-base poison: a store through an Unknown address leaves every cell"),
    Mutant("m68k_memory_push_at_a7", M68K,
           "      out.emplace_back(m68k_points_to_add(in.address[7], {-static_cast<std::int64_t>(write.span)}), write.span);",
           "      out.emplace_back(in.address[7], write.span);",
           (MEMORY_TEST,), "excluded stack writer that actually aliases: a push is placed at A7 instead of below it"),
    Mutant("m68k_memory_async_writer_ignored", M68K_MEMORY,
           "    if (policy.asynchronous(cell)) return fail(Sub::async_writer);\n",
           "",
           (MEMORY_TEST, FRAMES_TEST), "ignored interrupt-handler writer: a read of an asynchronously written cell is precise"),
    # ---------------------------------------------------------------- M68K call contexts and summaries (SEG-030-T005)
    Mutant("m68k_stale_summary_accepted", M68K,
           "      valid = false;  // the used summary is below what its own solution derives (stale): grow it\n",
           "",
           (CONTEXTS_TEST,), "a round whose used summary is exceeded by a newly discovered writer still validates (stale summary)"),
    Mutant("m68k_stale_summary_kept", M68K,
           "      out.next.summaries.emplace(context, join(previous->second, *summary));\n",
           "      out.next.summaries.emplace(context, previous->second);\n",
           (CONTEXTS_TEST,), "an exceeded summary is kept instead of being regrown with the newly discovered writer"),
    Mutant("m68k_unbalanced_return_summarized", M68K,
           "      else facts.unbalanced = true;\n",
           "",
           (CONTEXTS_TEST,), "an RTS away from the entry stack delta does not prevent a summary"),
    Mutant("m68k_recursion_not_context_bound", M68K,
           "        if (!activation.fail && cyclic) activation.fail = Sub::context_bound;\n",
           "",
           (CONTEXTS_TEST,), "a recursive activation is not reported context_bound"),
    Mutant("m68k_writer_set_ignores_callee_contexts", M68K,
           "    report.max_cells = std::max(report.max_cells, state.memory.cells.size());\n"
           "    const auto decoded = adapter.decode(m68k_point_pc(point));\n",
           "    report.max_cells = std::max(report.max_cells, state.memory.cells.size());\n"
           "    const auto decoded = adapter.decode(static_cast<std::uint32_t>(point));\n",
           (CONTEXTS_TEST,), "the asynchronous-writer set skips the stores of callee-context points"),
    Mutant("m68k_summary_drops_memory_effects", M68K,
           "    state = summary->second;\n",
           "    state = summary->second;\n    state.memory = in.memory;\n",
           (CONTEXTS_TEST,), "an applied summary keeps the caller's pre-call memory instead of the callee's memory effects"),
    Mutant("m68k_exception_continuation_delta_bottom", M68K,
           "    state.stack_delta = continuation ? FiniteValue::unknown(UnknownReason::unsupported_transfer) : FiniteValue::of({0U});\n",
           "    state.stack_delta = continuation ? FiniteValue::bottom() : FiniteValue::of({0U});\n",
           (CONTEXTS_TEST,), "a resuming exception's continuation has a bottom stack delta that joins away (unbalanced path proven)"),
    Mutant("m68k_context_bound_raised", M68K_ADAPTER_HEADER,
           "inline constexpr std::size_t m68k_context_bound = 8U;",
           "inline constexpr std::size_t m68k_context_bound = 9U;",
           (CONTEXTS_TEST,), "raised context bound: K = 9 instead of ADR 0079 decision 11's K = 8 (SEG-030-T008)"),
    Mutant("m68k_query_reads_context_zero", M68K,
           "    out = join(out, state->values.values[m68k_analysis_slot(reg, width)]);\n",
           "    if (point == (pc & bus_mask)) out = join(out, state->values.values[m68k_analysis_slot(reg, width)]);\n",
           (CONTEXTS_TEST,), "the typed data-register query reads only the context-0 point of the PC"),
    # ---------------------------------------------------------------- M68K interrupt mask, handler instances and frames (SEG-030-T006)
    Mutant("m68k_unknown_sr_treated_as_masked", M68K_FRAMES,
           "  if (status.is_unknown()) return true;\n  if (!level || *level == 7U)",
           "  if (status.is_unknown()) return false;\n  if (!level || *level == 7U)",
           (FRAMES_TEST,), "dropping interrupt-mask poison: an Unknown SR is treated as masking every interrupt"),
    Mutant("m68k_preemption_at_equal_mask", M68K_FRAMES,
           "return m68k_status_mask(v) < *level; });",
           "return m68k_status_mask(v) <= *level; });",
           (FRAMES_TEST,), "incorrect handler nesting: an interrupt preempts a boundary whose mask equals its level"),
    Mutant("m68k_frame_at_a7", M68K,
           "  return m68k_points_to_add(a7, {-static_cast<std::int64_t>(m68k_exception_frame_bytes)});",
           "  return a7;",
           (FRAMES_TEST,), "wrong supervisor-stack provenance: the exception frame address is A7 instead of A7 - 6"),
    Mutant("m68k_reset_ssp_ignored", M68K,
           "      if (config_.frames.reset_ssp) state.address[7] = classify(image_, constant(*config_.frames.reset_ssp));\n",
           "",
           (FRAMES_TEST,), "wrong supervisor-stack provenance: the reset SSP is not the main flow's A7"),
    Mutant("m68k_rte_status_not_restored", M68K,
           "            edge.status = report->restored_status;",
           "            edge.status = out.status;",
           (FRAMES_TEST,), "incorrect RTE SR restoration: a proven RTE keeps the pre-RTE status"),
    Mutant("m68k_rte_without_frame", M68K,
           "  if (rte && !m68k_status_supervisor_proven(status)) return finish(UnknownReason::unsupported_transfer, Sub::frame_unproven);",
           "  if (false) return finish(UnknownReason::unsupported_transfer, Sub::frame_unproven);",
           (FRAMES_TEST,), "RTE proven without a frame context whose effective status proves supervisor mode"),
    Mutant("m68k_partition_policy_dropped", M68K,
           "  for (const auto &[tag, policy] : config_.frames.policies) tag_policies_.emplace(tag, join(config_.memory.policy, policy));",
           "  for (const auto &[tag, policy] : config_.frames.policies) tag_policies_.emplace(tag, config_.memory.policy);",
           (FRAMES_TEST,), "stale memory fact after policy growth: a partition's grown asynchronous writers are not applied"),
    Mutant("m68k_unanalysed_handler_precise", M68K,
           "  every_cell.async_all = true;\n",
           "",
           (FRAMES_TEST,), "the code of a handler without an analysed instance reads work RAM without any asynchronous writer"),
    Mutant("m68k_frame_integrity_ignored", M68K,
           "    bool intact = !subtree[tag].overlaps(0, 2);",
           "    bool intact = true;",
           (FRAMES_TEST,), "a handler rewriting its saved SR keeps the interrupted status"),
    Mutant("m68k_interrupt_resumption_unlabelled", M68K,
           "    out.sub = rte && tag != 0U ? Sub::interrupt_resumption : sub;",
           "    out.sub = sub;",
           (FRAMES_TEST,), "a handler RTE resuming interrupted code is not reported interrupt_resumption"),
    # SEG-030-T008 differential finding: every boundary of every partition is a taking point, and an unmodelled taking point enters
    # its handler with an Unknown entry.
    Mutant("m68k_non_resuming_instance_not_preemptible", M68K,
           "  if (!frames.instances.contains(parent_tag)) return Unanalysed::unmodelled_parent;\n",
           "  if (!frames.instances.contains(parent_tag) || !frames.instances.at(parent_tag).resuming) return Unanalysed::unmodelled_parent;\n",
           (FRAMES_TEST,), "a non-resuming instance is not preemptible: no child instance is entered at its frame address",
           "correction cycle 2 makes every synchronous handler potentially resuming because any handler may execute RTE; "
           "there is no longer a non-resuming instance for this historical guard to exclude"),
    Mutant("m68k_unanalysed_taking_entry_dropped", M68K,
           "    if (credited) unknown_entries.insert(handler);\n",
           "    (void)handler;\n",
           (FRAMES_TEST,), "a handler taken at an unanalysed point keeps only the analysed instances' precise entries"),
    Mutant("m68k_unknown_entry_partition_not_taker", M68K,
           "    const bool unmodelled = tag == m68k_unknown_entry_tag;\n",
           "    const bool unmodelled = false;\n",
           (FRAMES_TEST,), "a vector raised or taken inside the unknown-entry partition does not enter its handler"),
    Mutant("m68k_undelivered_interrupt_ignored", M68K,
           "    if (m68k_vector_class(vector.vector) == M68kVectorClass::interrupt) sources.emplace_back(vector, false);\n",
           "    (void)vector;\n",
           (FRAMES_TEST, PREMISE_TEST), "an installed but undelivered interrupt handler is not a potential asynchronous writer"),
    Mutant("m68k_writer_only_points_credited", M68K,
           "      if (!credited(point)) continue;\n",
           "",
           (FRAMES_TEST, PREMISE_TEST), "a writer-only (undelivered-interrupt) instance's points are credited to D and the site reports"),
    Mutant("genesis_premise_drops_level_4", GENESIS_PREMISE,
           "genesis_main_cpu_interrupt_levels{2U, 4U, 6U};",
           "genesis_main_cpu_interrupt_levels{2U, 6U, 6U};",
           (PREMISE_TEST,), "the Genesis premise omits the H-int (level 4) source"),
    Mutant("genesis_premise_drops_level_2", GENESIS_PREMISE,
           "genesis_main_cpu_interrupt_levels{2U, 4U, 6U};",
           "genesis_main_cpu_interrupt_levels{4U, 4U, 6U};",
           (PREMISE_TEST,), "the Genesis premise omits the external (level 2) source"),
    Mutant("genesis_premise_admits_level_7", GENESIS_PREMISE,
           "  if (premise == GenesisInterruptPremise::unconfigured) return true;\n",
           "  if (premise == GenesisInterruptPremise::unconfigured || vector == 31U) return true;\n",
           (PREMISE_TEST,), "the Genesis premise admits a level-7 source the board never asserts"),
    Mutant("m68k_summary_absolute_exit_a7", M68K,
           "    if (config_.domains.frames) state.address[7] = in.address[7];\n  } else {",
           "  } else {",
           (FRAMES_TEST,), "a balanced summary's continuation keeps the joined absolute exit A7 instead of the caller's A7"),
    Mutant("m68k_unbalanced_callee_rebased", M68K,
           "    state = opaque_continuation(opaque == contexts.opaque.end() ? Sub::none : opaque->second, tag);\n",
           "    state = opaque_continuation(opaque == contexts.opaque.end() ? Sub::none : opaque->second, tag);\n"
           "    state.address[7] = in.address[7];\n",
           (FRAMES_TEST,), "an unproven (unbalanced) callee's continuation is rebased to the caller's A7"),
    Mutant("m68k_unmodelled_always_raise_unknown_effect", M68K,
           "    if (result.solution.unresolved_computed.contains(point) ||\n",
           "    if (result.solution.unresolved_computed.contains(point) || (control.always_raises_exception && !modelled_raise) ||\n",
           (FRAMES_TEST,), "an unconfigured always-raising instruction is treated as an opaque effect instead of a terminal path"),
    Mutant("m68k_unmodelled_trap_terminates_path", M68K,
           "        (control.stacked == M68kStackedContinuationKind::exception_continuation && !config.exception_continuations &&\n"
           "         !(modelled_raise && decoded->operation.kind == M68kIrKind::trap_exception)))\n",
           "        (control.stacked == M68kStackedContinuationKind::exception_continuation && !config.exception_continuations &&\n"
           "         decoded->operation.kind != M68kIrKind::trap_exception &&\n"
           "         !(modelled_raise && decoded->operation.kind == M68kIrKind::trap_exception)))\n",
           (FRAMES_TEST,), "a TRAP whose resuming continuation is not modelled is treated as a path terminator (escape dropped)"),
    Mutant("m68k_whole_program_status_bound", M68K,
           "  return found == config_.frames.status_bounds.end() ? FiniteValue::bottom() : found->second;\n",
           "  FiniteValue all;\n"
           "  for (const auto &entry : config_.frames.status_bounds) all = join(all, entry.second);\n"
           "  return found == config_.frames.status_bounds.end() ? all : all;\n",
           (FRAMES_TEST,), "an opaque continuation takes the whole-program status bound (another partition's lower mask leaks in)"),
    # ---------------------------------------------------------------- Z80 projection and adapter
    Mutant("z80_ram_load_precise", Z80_ADAPTER,
           "        if (!image_.contains(at)) return FiniteValue::unknown(UnknownReason::non_immutable_read);",
           "        if (!image_.contains(at)) continue;",
           (Z80_TEST,), "a load outside the immutable image is treated as a precise value"),
    Mutant("z80_unknown_address_load_bottom", Z80_ADAPTER,
           "    if (!addresses.is_precise()) return FiniteValue::unknown(UnknownReason::non_immutable_read);",
           "",
           (Z80_TEST,), "a load through an Unknown address yields bottom instead of Unknown"),
    Mutant("z80_store_into_image_accepted", Z80_ADAPTER,
           "  if (effect.store && eval.contradicts_premise(*effect.store)) return unresolved(UnknownReason::unsupported_transfer);",
           "  if (effect.store && eval.contradicts_premise(*effect.store) && false) return {};",
           (Z80_TEST,), "a precise store into the immutable image is accepted"),
    Mutant("z80_unsupported_invents_fallthrough", Z80_ADAPTER,
           "  if (!effect.supported) return unresolved(UnknownReason::unsupported_transfer);",
           "  if (!effect.supported) {\n    core::TransferResult<State> invented;\n"
           "    invented.edges.push_back({(point + 1U) & kAddressMask, core::EdgeKind::fallthrough, in});\n"
           "    return invented;\n  }",
           (Z80_TEST,), "an unsupported form gets an invented one-byte fallthrough"),
    Mutant("z80_projection_unsupported_as_nop", Z80_EFFECTS,
           "    if (!effect_.supported) return Z80Effect{};",
           "    if (!effect_.supported) {\n      Z80Effect nop{};\n      nop.supported = true;\n"
           "      nop.fallthrough = effect_.fallthrough;\n      return nop;\n    }",
           (Z80_TEST,), "the projection reports every unsupported form as a NOP"),
    Mutant("z80_half_to_pair_desync", Z80_ADAPTER,
           "  if (!is_half(target)) return;\n  const bool high = is_high(target);",
           "  return;\n  const bool high = is_high(target);",
           (Z80_TEST,), "writing an 8-bit half never updates its pair"),
    Mutant("z80_pair_to_half_desync", Z80_ADAPTER,
           "    out.values[index(high)] = value.map([](std::uint64_t p) { return extract(p, true); }, bound);\n"
           "    out.values[index(other_half(high))] = value.map([](std::uint64_t p) { return extract(p, false); }, bound);\n",
           "    (void)high;\n",
           (Z80_TEST,), "writing a pair never updates its halves"),
    Mutant("z80_half_pointwise_wrong_half", Z80_ADAPTER,
           "            return insert(p, is_high(target), byte);",
           "            return insert(p, !is_high(target), byte);",
           (Z80_TEST,), "the exact pointwise half update writes the other half of the pair"),
    Mutant("z80_computed_from_unknown", Z80_ADAPTER,
           "        result.unresolved_computed = targets.reason();",
           "        result.edges.push_back({effect.fallthrough, EdgeKind::computed, out});",
           (Z80_TEST,), "JP (HL) with an Unknown HL invents a computed edge instead of an unresolved site"),
    Mutant("z80_call_continuation_keeps_state", Z80_ADAPTER,
           "          {effect.fallthrough, EdgeKind::return_edge, State::all_unknown(UnknownReason::unsupported_transfer)});",
           "          {effect.fallthrough, EdgeKind::return_edge, out});",
           (Z80_TEST,), "a CALL continuation keeps the post-call state (no call summary exists)"),
    Mutant("z80_conditional_drops_fallthrough", Z80_ADAPTER,
           "      result.edges.push_back({effect.target, EdgeKind::branch, out});\n"
           "      result.edges.push_back({effect.fallthrough, EdgeKind::fallthrough, out});\n",
           "      result.edges.push_back({effect.target, EdgeKind::branch, out});\n",
           (Z80_TEST,), "a conditional jump loses its not-taken edge"),
    Mutant("z80_djnz_no_decrement", Z80_EFFECTS,
           "        write(Reg::b, add(Reg::b, -1));\n",
           "",
           (Z80_TEST,), "DJNZ does not decrement B"),
    Mutant("z80_alu_keeps_a", Z80_EFFECTS,
           "        if (is_alu(m) && d == O::a && (s == O::r || s == O::n || s == O::hl_ind)) {\n"
           "          if (m != Mnemonic::cp) write(Reg::a, opaque());",
           "        if (is_alu(m) && d == O::a && (s == O::r || s == O::n || s == O::hl_ind)) {\n"
           "          if (m == Mnemonic::cp) write(Reg::a, opaque());",
           (Z80_TEST,), "ALU forms keep a precise A (and CP clobbers it)"),
    # ---------------------------------------------------------------- SEG-030-T010: Genesis Z80 work-RAM store-freedom proof
    Mutant("z80_proof_cannot_store_ram", Z80_PROOF,
           "        if (bank >= genesis_z80_bank_ram_first) result.work_ram.insert(((bank << 15U) | (address & 0x7FFFU)) & 0xFFFFU);\n",
           "",
           (Z80_PROOF_TEST,), "assumes the Z80 cannot store into 68K RAM through the bank window"),
    Mutant("z80_proof_bank_change_ignored", Z80_PROOF,
           "        s.bank_known = known;\n        s.bank_value = static_cast<std::uint16_t>(value & known);\n",
           "        (void)known;\n        (void)value;\n",
           (Z80_PROOF_TEST,), "a bank-register write never changes the latch"),
    Mutant("z80_proof_reset_interval_ignored", Z80_PROOF,
           "    const bool running = !group.held_in_reset;",
           "    const bool running = false;",
           (Z80_PROOF_TEST,), "every 68K store counts as held in reset (the BUSREQ/RESET interval is ignored)"),
    Mutant("z80_proof_reset_interval_never_held", Z80_PROOF,
           "    const bool running = !group.held_in_reset;",
           "    const bool running = true;",
           (Z80_PROOF_TEST,), "no 68K store counts as held in reset (the image-construction interval is ignored)"),
    Mutant("z80_proof_window_store_unclassified", Z80_PROOF,
           "    } else if (address >= window_first) {",
           "    } else if (address >= window_first && false) {",
           (Z80_PROOF_TEST,), "a store target at or above $8000 is not classified as a bank-window store"),
    Mutant("z80_proof_stack_push_unclassified", Z80_PROOF,
           "      if (transfer.stores.empty()) continue;",
           "      if (transfer.stores.empty() || transfer.stores.front().slot) continue;",
           (Z80_PROOF_TEST,), "a stack push (SP in the window) is not a classified store"),
    Mutant("z80_proof_m68k_operand_rewrite_allowed", Z80_PROOF,
           "        if (result.control_bytes.contains(byte)) proof.reasons.insert(Reason::m68k_store_into_z80_code);",
           "        (void)byte;",
           (Z80_PROOF_TEST,), "a 68K store may rewrite a Z80 operand byte while the Z80 may run"),
    Mutant("z80_proof_ei_shadow_permanent", Z80_PROOF,
           "    if (s.iff & 4U) s.iff = static_cast<std::uint8_t>((s.iff & ~4U) | 2U);",
           "",
           (Z80_PROOF_TEST,), "interrupts enabled by EI are never accepted"),
    Mutant("z80_proof_bound_ignored_by_m68k", M68K,
           "    if (!config.memory.external_writer_bound) derived.external_writer = true;",
           "    if (true) derived.external_writer = true;",
           (Z80_PROOF_TEST,), "the M68K memory domain ignores the credited bound (blanket external writer)"),
    Mutant("z80_proof_bound_ranges_dropped", M68K,
           "      for (const auto &range : *config.memory.external_writer_bound) derived.add_async(range);",
           "      (void)config.memory.external_writer_bound;",
           (Z80_PROOF_TEST,), "a `ranges` bound credits no Z80 writer at all"),
    Mutant("z80_proof_bound_not_validated", REPORT,
           "      if (!used || covered || report.z80_proof_runs >= 4U) {",
           "      if (true) {\n        covered = true;",
           (Z80_PROOF_TEST,), "the optimistic bound is credited without validating it against the run's own 68K stores"),
    # SEG-030-T008 part 2: the return slot of an RTS at the entry stack delta (ADR 0079 decision 8).
    Mutant("m68k_return_slot_precise_rewrite_ignored", M68K,
           "    if (std::all_of(values->begin(), values->end(), [&](std::uint32_t v) { return expected.contains(v & bus_mask); })) return out;",
           "    if (recorded) return out;",
           (RETURN_SLOT_TEST,), "a precise rewrite of a recorded return slot is ignored (the RTS stays a normal return)"),
    Mutant("m68k_return_slot_weak_rewrite_normal", M68K,
           "  if (read.known || (recorded && rewritten)) return unknown(UnknownReason::unsupported_transfer, Sub::return_slot_rewritten);",
           "  if (read.known) return unknown(UnknownReason::unsupported_transfer, Sub::return_slot_rewritten);",
           (RETURN_SLOT_TEST,), "a weak rewrite of a return slot is treated as a normal return"),
    Mutant("m68k_return_slot_store_not_related", M68K_MEMORY,
           "      if (range.kind == cell.kind && range.id == cell.id && range.lo < hi && lo < range.hi) slot.rewritten = true;",
           "      (void)range, (void)lo, (void)hi;",
           (RETURN_SLOT_TEST,), "a known-target store is never related to the recorded return slots"),
    # SEG-030-T009 correction cycle: handler register resumptions (ADR 0079 decisions 7 and 8).
    Mutant("m68k_resumption_exit_not_joined", M68K,
           "        if (resumption.data[slot].is_bottom()) continue;",
           "        if (true) continue;",
           (FRAMES_TEST,), "a resuming handler's exit data registers are not joined into the boundaries where it can be taken"),
    Mutant("m68k_unproven_resumption_preserving", M68K,
           "    if (resumption.unproven) {",
           "    if (false) {",
           (FRAMES_TEST,), "an unanalysed/unproven handler resumption is treated as preserving every register"),
    Mutant("m68k_nested_resumption_not_propagated", M68K,
            "  if (found == config_.frames.resumptions.end()) return false;",
            "  if (found == config_.frames.resumptions.end() || tag != 0U) return false;",
            (FRAMES_TEST,), "a nested child's register effects do not reach its parent handler's exit (only the main flow joins)"),
    # SEG-030-T009 correction cycle 2: hardware-frame PC provenance and synchronous resumption edges.
    Mutant("m68k_saved_frame_pc_rewrite_ignored", M68K,
            "        out.saved[key] = frame_pc_identity;\n        const auto offset = frame_pc_fact_offset(found->second);",
            "        out.saved[key] = found->second;\n        const auto offset = frame_pc_fact_offset(found->second);",
            (FRAMES_TEST,), "a long replacement of the saved frame PC is ignored and credited as unchanged"),
    Mutant("m68k_frame_pc_offset_treated_as_zero", M68K,
            "  return offset;\n}\n\n// ---------------------------------------------------------------------------------------------------------------\n// Adapter.",
            "  return 0;\n}\n\n// ---------------------------------------------------------------------------------------------------------------\n// Adapter.",
            (FRAMES_TEST,), "every exact saved-PC increment/decrement is treated as an unchanged frame"),
    Mutant("m68k_trap_resumption_drops_handler_effect", M68K,
            "        if (!apply_resumptions(tag, state, &in, 1U << level, offset, false)) continue;",
            "        if (!config_.frames.resumptions.contains(tag)) continue;",
            (FRAMES_TEST,), "a TRAP/fault resumption edge omits the handler's register effects"),
    Mutant("m68k_fault_resumes_next", M68K,
            "        const auto stacked = level == m68k_level_fault ? pc : next;",
            "        const auto stacked = next;",
            (FRAMES_TEST,), "illegal/line-A/line-F/privilege faults resume at the next instruction instead of the faulting PC"),
    Mutant("m68k_trap_resumes_current", M68K,
            "        const auto stacked = level == m68k_level_fault ? pc : next;",
            "        const auto stacked = level == m68k_level_fault || level == m68k_level_trap ? pc : next;",
            (FRAMES_TEST,), "TRAP #n resumes at its instruction instead of the stacked next PC"),
    Mutant("m68k_unknown_frame_pc_rewrite_credited", M68K,
            "  const auto offset = frame_pc_fact_offset(found->second);\n  if (!offset) return std::nullopt;",
            "  const auto offset = frame_pc_fact_offset(found->second);\n  if (!offset) return 0;",
            (FRAMES_TEST,), "an unknown/replacement frame-PC value is credited as an unchanged hardware frame"),
    Mutant("m68k_offset_resumption_edge_omitted", M68K,
            "      if (offset != 0)\n        for (const auto &edge : result.edges) {",
            "      if (false)\n        for (const auto &edge : result.edges) {",
            (FRAMES_TEST,), "a proven non-zero interrupt frame-PC offset emits no resumed edge"),
    Mutant("m68k_frames_fallback_marked_complete", M68K,
            "    out.complete = false;",
            "    out.complete = true;",
            (FRAMES_TEST,), "a historical contexts fallback is exposed as a complete requested frames result"),
    # ---- SEG-034 (ADR 0081): whole-image-trigger precision refinements ----
    Mutant("m68k_spill_resolution_disabled", M68K,
           "  if (config_.domains.memory)\n    for (std::size_t i = 0; i < targets.size(); ++i) {\n      auto resolved = resolve_store_spill",
           "  if (false)\n    for (std::size_t i = 0; i < targets.size(); ++i) {\n      auto resolved = resolve_store_spill",
           (PRECISION_TEST,), "a store spilling past its region end is no longer resolved (fail-closed baseline: a precision regression)"),
    Mutant("m68k_spill_landing_dropped", M68K,
           "    pairs.emplace_back(landing, M68kOffsetSet::of({static_cast<std::uint32_t>(start)}));",
           "    (void)landing;",
           (PRECISION_TEST,), "the bytes a spilling store lands on in the next region are not written (unsound)"),
    Mutant("m68k_spill_clipped_member_dropped", M68K,
           "        exact.push_back(last);  // keep the clipped member",
           "        (void)last;",
           (PRECISION_TEST,), "the clipped in-region bytes of a spilling exact member are dropped from the target set (unsound)"),
    Mutant("m68k_spill_untracked_landing_dropped", M68K,
           "    pairs.emplace_back(landing, M68kOffsetSet::of({static_cast<std::uint32_t>(start)}));",
           "    if (m68k_memory_tracked(next->kind)) pairs.emplace_back(landing, M68kOffsetSet::of({static_cast<std::uint32_t>(start)}));",
           (PRECISION_TEST,), "the bytes a spill lands on in an untracked region are hidden from the release and observed-store consumers (unsound)"),
    Mutant("m68k_spill_value_joined", M68K,
           "    if (i < spilled.size() && spilled[i]) value.reset();",
           "    (void)spilled;",
           (PRECISION_TEST,), "a spilling store's value is joined into the cell of its clipped member (unsound)"),
    Mutant("m68k_return_slot_synthetic_target_records", M68K_MEMORY,
           "  if (return_address && !synthetic_target && span == 4U && targets.is_exact()",
           "  if (return_address && !(synthetic_target && false) && span == 4U && targets.is_exact()",
           (PRECISION_TEST,), "a spill-resolved target records a precise return slot for a return-address writer (unsound)"),
    Mutant("m68k_return_slot_spill_flag_dropped", M68K,
           "                            i < spilled.size() && spilled[i]);",
           "                            false);",
           (PRECISION_TEST,), "the adapter does not tell the return-slot recorder that a target was spill-resolved",
           equivalent="a return-address writer is a push to A7-4 and an A7 offset never exceeds its region size, so a push target never "
                      "spills and is never spill-resolved; the flag is defence in depth, and the recorder's own behaviour is killed directly"),
    Mutant("m68k_spill_inside_part_dropped", M68K,
           "    pairs.emplace_back(region, std::move(inside));",
           "    (void)inside;",
           (PRECISION_TEST,), "the in-region part of a partially spilling store is dropped (unsound)"),
    # ---- SEG-031 (ADR 0080): hybrid admission safety properties (`--group hybrid`) ----
    Mutant("hybrid_island_member_omitted", HYBRID_PLAN,
           "for (std::uint64_t offset = offsets.lo(); offset <= offsets.hi(); offset += offsets.stride())",
           "for (std::uint64_t offset = offsets.lo(); offset < offsets.hi(); offset += offsets.stride())",
           (HYBRID_PLAN_TEST,), "an island omits the last member of its proven strided set", group="hybrid"),
    Mutant("hybrid_closure_skipped", HYBRID_PLAN,
           "    if (grown == islands && grown_opaque == opaque) {",
           "    if (round > 1U || (grown == islands && grown_opaque == opaque)) {",
           (HYBRID_PLAN_TEST,), "the closure stops after the first island round without re-analysing island code", group="hybrid"),
    Mutant("hybrid_island_code_transfers_ignored", HYBRID_PLAN,
           "    for (const auto pc : uncovered_sites(report)) {",
           "    for (const auto pc : round == 1U ? uncovered_sites(report) : std::set<std::uint32_t>{}) {",
           (HYBRID_PLAN_TEST,), "uncovered transfers introduced by island code are ignored", group="hybrid"),
    Mutant("hybrid_island_shrunk_to_reached", HYBRID_PLAN,
           "      else merge_into(grown[pc], site.entries);",
           "      else { std::vector<std::uint32_t> seen; for (const auto e : site.entries) if (report.discovered.contains(e)) "
           "seen.push_back(e); merge_into(grown[pc], seen); }",
           (HYBRID_PLAN_TEST,), "an island is shrunk to what was already reached (evidence-style shrinking)", group="hybrid"),
    Mutant("hybrid_operand_width_bound", HYBRID_PLAN,
           "  if (!value.is_known() || value.width_derived) return site;",
           "  if (!value.is_known()) return site;",
           (HYBRID_PLAN_TEST,), "a width-derived pointer set bounds an island (the forbidden SEG-024 rule)", group="hybrid"),
    Mutant("hybrid_alias_mapping_ignored", HYBRID_PLAN,
           "      if ((target & 1U) == 0U && image.mapped(target)) entries.insert(target);",
           "      if ((target & 1U) == 0U && image.mapped(target) && target < 0xE00000U) entries.insert(target);",
           (HYBRID_PLAN_TEST,), "island members inside a static_proof RAM alias mapping are dropped", group="hybrid"),
    Mutant("hybrid_incomplete_closure_complete", HYBRID_PLAN,
           "  plan.outcome = GenesisHybridOutcome::broad_closure_bound;",
           "  plan.outcome = GenesisHybridOutcome::hybrid;",
           (HYBRID_PLAN_TEST,), "round-bound exhaustion is reported as a hybrid result", group="hybrid"),
    Mutant("hybrid_incomplete_solve_credited", HYBRID_PLAN,
           "      plan.outcome = GenesisHybridOutcome::broad_analysis_incomplete;\n      return plan;",
           "      plan.outcome = GenesisHybridOutcome::broad_analysis_incomplete;",
           (HYBRID_PLAN_TEST,), "an incomplete solve is not a broad fallback", group="hybrid"),
    Mutant("hybrid_unknown_provenance_not_widened", HYBRID_PLAN,
           "  if (!value.is_known() || value.width_derived) return site;",
           "  if (!value.is_known() || value.width_derived) { site.container = GenesisHybridContainer::points_to_region; return site; }",
           (HYBRID_PLAN_TEST,), "an Unknown pointer gets an empty island instead of widening to broad", group="hybrid"),
    Mutant("hybrid_whole_image_ignored", HYBRID_PLAN,
           "      if (site.container == GenesisHybridContainer::whole_image) whole = true;",
           "      if (site.container == GenesisHybridContainer::whole_image) {}",
           (HYBRID_PLAN_TEST,), "an unbounded container does not degenerate to broad", group="hybrid"),
    Mutant("hybrid_materialized_roots_dropped", HYBRID_PLAN,
           "      merge_into(grown_opaque[*entry], materialized);",
           "      (void)materialized;",
           (HYBRID_PLAN_TEST,), "static_proof executable images are not admitted as mandatory roots", group="hybrid"),
    Mutant("hybrid_island_edges_dropped", M68K,
           "  if (const auto island = config_.island_entries.find(pc); island != config_.island_entries.end()) computed_edges(island->second);",
           "",
           (HYBRID_PLAN_TEST,), "the adapter never follows configured island targets (island code unanalysed)", group="hybrid"),
    Mutant("admission_fixed_successor_unchecked", ADMISSION,
           "      if (broad.contains(target) && !admitted(target)) return \"fixed_successor_not_admitted\";",
           "      (void)target;",
           (ADMISSION_TEST,), "production accepts a plan that omits a fixed successor (cross-island direct edge)", group="hybrid"),
    Mutant("admission_vector_root_unchecked", ADMISSION,
           "    if (broad.contains(root & bus_mask) && !admitted(root)) return \"machine_root_not_admitted\";",
           "    (void)root;",
           (ADMISSION_TEST,), "production accepts a plan that drops a reset/vector root", group="hybrid"),
    Mutant("admission_materialized_unchecked", ADMISSION,
           "      if (entry->execution_alias) return \"materialized_image_not_admitted\";",
           "",
           (ADMISSION_TEST,), "production accepts a plan that drops a static_proof alias identity", group="hybrid"),
    Mutant("admission_range_overshoot", ADMISSION,
           "    const std::uint32_t end = address + 2U;",
           "    const std::uint32_t end = address + 4U;",
           (ADMISSION_TEST,), "plan ranges admit a broad identity outside H", group="hybrid"),
    Mutant("admission_universe_unchecked", ADMISSION,
           "    if (genesis_hybrid_admission_universe_digest(universe) != plan.universe_sha256) return \"universe_mismatch\";",
           "    (void)universe;",
           (ADMISSION_TEST,), "production applies a plan computed for another broad universe", group="hybrid"),
    Mutant("hybrid_validator_unexpected_site_passes", HYBRID_PLAN,
           "    if (control.dynamic != M68kDynamicControlFamily::return_from_subroutine) return \"unclassified_dynamic_site\";",
           "",
           (HYBRID_PLAN_TEST,), "the validator passes a reached dynamic site that is neither uncovered nor resolved", group="hybrid"),
    Mutant("admission_alias_set_unchecked", ADMISSION,
           "    return \"alias_set_mismatch\";",
           "    {}",
           (ADMISSION_TEST,), "production applies a plan computed for another executable-image set", group="hybrid"),

]


def source_text(raw: bytes) -> str:
    """The one textual form both the stale-pattern preflight and mutation use: UTF-8 with universal-newline normalization, so a
    CRLF checkout (Windows) presents the same `\n` text the mutant patterns are authored in."""
    return io.TextIOWrapper(io.BytesIO(raw), encoding="utf-8").read()


def apply_edit(text: str, mutant: Mutant) -> str:
    count = text.count(mutant.old)
    if count != 1:
        raise StaleMutant(f"stale mutant {mutant.name}: edit pattern occurs {count} times in {mutant.path} (expected exactly 1)")
    return text.replace(mutant.old, mutant.new, 1)


def self_check_stale_detection() -> None:
    probe = Mutant("probe", "probe", "needle", "pin", (), "probe")
    for text in ("no match here", "needle and needle"):
        try:
            apply_edit(text, probe)
        except StaleMutant:
            continue
        raise SystemExit(f"FAIL self-check: stale detection accepted {text!r}")
    if apply_edit("a needle", probe) != "a pin":
        raise SystemExit("FAIL self-check: a unique pattern was not applied")
    # LF and CRLF sources are equivalent once read through source_text; zero and multiple matches stay stale on both.
    lines = Mutant("probe_lines", "probe", "one;\ntwo;\n", "one;\n", (), "probe")
    for newline in (b"\n", b"\r\n"):
        def raw(*parts: bytes) -> bytes:
            return b"".join(part + newline for part in parts)
        if apply_edit(source_text(raw(b"x;", b"one;", b"two;")), lines) != "x;\none;\n":
            raise SystemExit(f"FAIL self-check: a multiline pattern was not applied to {newline!r} source")
        for text in (raw(b"one;", b"three;"), raw(b"one;", b"two;", b"one;", b"two;")):
            try:
                apply_edit(source_text(text), lines)
            except StaleMutant:
                continue
            raise SystemExit(f"FAIL self-check: stale detection accepted {newline!r} source {text!r}")
    names = [m.name for m in MUTANTS]
    if len(names) != len(set(names)):
        raise SystemExit("FAIL self-check: duplicate mutant names")
    for m in MUTANTS:
        if m.old == m.new or m.group not in GROUP_TESTS or not set(m.tests) <= set(GROUP_TESTS[m.group]):
            raise SystemExit(f"FAIL self-check: malformed mutant {m.name}")
    print("ok    self-check: stale edit patterns (missing, duplicated) are rejected; LF and CRLF sources are equivalent")


class SourceStamper:
    """Incremental-rebuild timestamp discipline: a mutation or a restoration can never be skipped by the build tool. No sleeping.

    make and Ninja rebuild a target only when a prerequisite is strictly newer than it; GNU Make 3.81 (and any coarse filesystem)
    compares whole seconds. Two holes follow when a mutate/build/restore cycle is faster than the timestamp tick:
      1. a source written in the same tick as the previous build's outputs is not newer than its object, so it is not recompiled;
      2. even when the object is recompiled, the archive and executable linked by the previous build in that same tick are not
         older than it, so the archive/relink is skipped and the stale executable runs.
    `write` closes hole 1: after every mutation and restoration it stamps the file with
    `max(previous stamp, now, the file's own mtime, newest mtime anywhere in the build tree) + STEP_NS`; the stamp is kept across
    operations so stamps strictly increase even within one wall-clock second and exceed every output (and the build tool's own
    logs) the source is compared against. `settle`, called immediately before every build, closes hole 2: when the previous build
    of the WHOLE fixture set succeeded (every output up to date, so no out-of-date relation can be hidden), it back-dates every
    build-tree file to one whole second strictly older than the current second and than every source stamp written since, so
    every output of the coming build is strictly newer than every older output, and every stamped source stays newer than them.
    """

    STEP_NS = 2_000_000_000  # > 1 s so a coarse (1 s or 2 s FAT-style) filesystem still orders the stamp strictly after outputs
    SECOND_NS = 1_000_000_000

    def __init__(self, build: pathlib.Path) -> None:
        self.build = build
        self.stamp = 0
        self.pending: list[int] = []  # stamps written since the last settle
        self.consistent = False  # the last build of the whole fixture set succeeded

    def build_files(self) -> list[str]:
        files = []
        pending = [str(self.build)]
        while pending:
            with os.scandir(pending.pop()) as entries:
                for entry in entries:
                    files.append(entry.path)
                    if entry.is_dir(follow_symlinks=False):
                        pending.append(entry.path)
        return files

    def newest_output_ns(self) -> int:
        newest = 0
        for file in self.build_files():
            try:
                newest = max(newest, os.lstat(file).st_mtime_ns)
            except OSError:
                continue
        return newest

    def write(self, path: pathlib.Path, data: bytes) -> int:
        """Write a mutation or restoration and stamp it strictly after every build output and every earlier stamp."""
        path.write_bytes(data)
        self.stamp = max(self.stamp, time.time_ns(), path.stat().st_mtime_ns, self.newest_output_ns()) + self.STEP_NS
        os.utime(path, ns=(self.stamp, self.stamp))
        self.pending.append(self.stamp)
        return self.stamp

    def settle(self) -> None:
        """Back-date a fully up-to-date build tree below the current second (and below every pending stamp) before a build."""
        if self.consistent:
            floor = min([time.time_ns()] + self.pending) // self.SECOND_NS * self.SECOND_NS - self.SECOND_NS
            for file in self.build_files():
                try:
                    if os.lstat(file).st_mtime_ns > floor:
                        set_mtime_no_follow(file, floor)
                except OSError:
                    continue
        self.pending.clear()

    def built(self, complete_and_successful: bool) -> None:
        """Record whether the build just finished left every output of the whole fixture set up to date."""
        self.consistent = complete_and_successful


def set_mtime_no_follow(path: str | os.PathLike[str], timestamp_ns: int) -> None:
    """Set a build entry's mtime without dereferencing links where the platform supports it.

    CPython on Windows rejects ``follow_symlinks=False`` for ``os.utime``. Mutation builds contain ordinary files and directories,
    so use the portable call there, but continue to skip a link rather than changing its target if one is encountered.
    """
    try:
        os.utime(path, ns=(timestamp_ns, timestamp_ns), follow_symlinks=False)
    except NotImplementedError:
        if not os.path.islink(path):
            os.utime(path, ns=(timestamp_ns, timestamp_ns))


def executable_stamps(executables: list[pathlib.Path]) -> dict[pathlib.Path, tuple[int, int]]:
    stamps = {}
    for executable in executables:
        st = executable.stat()
        stamps[executable] = (st.st_mtime_ns, st.st_size)
    return stamps


def rebuilt(before: dict[pathlib.Path, tuple[int, int]]) -> bool:
    """True when at least one of the executables was relinked by the last build (the changed source was really recompiled)."""
    return any(executable.stat().st_mtime_ns != stamp[0] or executable.stat().st_size != stamp[1]
               for executable, stamp in before.items())


def run(command: list[str], cwd: pathlib.Path | None = None, timeout: float = 1800) -> subprocess.CompletedProcess:
    return subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                          timeout=timeout)


def find_executable(build: pathlib.Path, name: str) -> pathlib.Path:
    suffix = ".exe" if os.name == "nt" else ""
    for candidate in (build / "tests" / (name + suffix), build / "tests" / "Debug" / (name + suffix)):
        if candidate.is_file():
            return candidate
    found = sorted(build.rglob(name + suffix))
    if not found:
        raise SystemExit(f"FAIL: built test executable {name} not found under {build}")
    return found[0]


def first_failure_line(output: str) -> str:
    for line in output.splitlines():
        if "FAIL" in line:
            return line.strip()[:140]
    lines = [line for line in output.splitlines() if line.strip()]
    return lines[-1].strip()[:140] if lines else ""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("product_root", type=pathlib.Path)
    parser.add_argument("cmake")
    parser.add_argument("c_compiler")
    parser.add_argument("cxx_compiler")
    parser.add_argument("--generator", default="", help="CMake generator of the temporary build (default: Ninja when found)")
    parser.add_argument("--make-program", default="", help="build tool of that generator (CMAKE_MAKE_PROGRAM)")
    parser.add_argument("--only", default="")
    parser.add_argument("--group", default="analysis", choices=sorted(GROUP_TESTS),
                        help="mutant group: analysis (SEG-029/SEG-030, default) or hybrid (SEG-031)")
    parser.add_argument("--keep", action="store_true", help="keep the temporary build directory")
    args = parser.parse_args()
    root = args.product_root.resolve()
    started = time.monotonic()

    self_check_stale_detection()
    fixtures = GROUP_TESTS[args.group]
    selected = [m for m in MUTANTS if m.group == args.group]
    if args.only:
        wanted = set(args.only.split(","))
        unknown = wanted - {m.name for m in selected}
        if unknown:
            raise SystemExit(f"FAIL: unknown mutant(s) {sorted(unknown)} in group {args.group}")
        selected = [m for m in selected if m.name in wanted]
    # Stale detection against the real sources before any build: every edit must match exactly once.
    stale = []
    for m in selected:
        try:
            apply_edit(source_text((root / m.path).read_bytes()), m)
        except StaleMutant as error:
            stale.append(str(error))
    if stale:
        print("\n".join("FAIL  " + s for s in stale))
        return 1
    print(f"ok    {len(selected)} mutant edit pattern(s) match the current sources exactly once")

    work = pathlib.Path(tempfile.mkdtemp(prefix="segarecomp-mutation-"))
    try:
        source = work / "src"
        build = work / "build"
        ignored_at_root = {"build", ".git", "games", ".tools", ".cache"}

        def ignore(directory: str, names: list[str]) -> list[str]:
            here = pathlib.Path(directory)
            return [n for n in names if n == "__pycache__" or (n in ignored_at_root and here.resolve() == root) or
                    (here / n / "CMakeCache.txt").is_file()]  # any other build tree

        shutil.copytree(root, source, ignore=ignore)
        configure = [args.cmake, "-S", str(source), "-B", str(build), "-DCMAKE_BUILD_TYPE=", "-DBUILD_TESTING=ON",
                     "-DSEGARECOMP_WARNINGS_AS_ERRORS=ON", f"-DCMAKE_C_COMPILER={args.c_compiler}",
                     f"-DCMAKE_CXX_COMPILER={args.cxx_compiler}"]
        generator = args.generator or ("Ninja" if shutil.which("ninja") else "")
        if generator:
            configure += ["-G", generator]
        if args.make_program:
            configure.append(f"-DCMAKE_MAKE_PROGRAM={args.make_program}")
        result = run(configure)
        if result.returncode != 0:
            print(result.stdout[-4000:])
            print("FAIL  configure of the temporary copy")
            return 1
        jobs = str(max(1, min(8, os.cpu_count() or 1)))

        stamper = SourceStamper(build)

        # Always the whole fixture set: a successful build then leaves every output up to date, which is what lets the stamper
        # settle (back-date) the tree soundly before the next build (see SourceStamper).
        # `watch`: fixture executables that must be relinked (the written source change must really reach them); the snapshot is
        # taken after settling, so `relinked` reflects the build alone.
        def build_tests(watch: tuple[str, ...] = ()) -> tuple[subprocess.CompletedProcess, bool]:
            stamper.settle()
            before = executable_stamps([find_executable(build, t) for t in watch])
            result = run([args.cmake, "--build", str(build), "--config", "Debug", "--parallel", jobs, "--target", *fixtures])
            stamper.built(result.returncode == 0)
            return result, (not watch or rebuilt(before))

        # Tests run in the listed order; with `stop_at_kill` the first failing test decides (a mutant can make a later fixture slow,
        # e.g. a join defect that oscillates until the solver's iteration bound). A timeout is an observable failure (a kill).
        def run_tests(tests: tuple[str, ...], stop_at_kill: bool = False) -> dict[str, tuple[bool, str]]:
            outcome = {}
            for test in tests:
                executable = find_executable(build, test)
                try:
                    limit = FIXTURE_TIMEOUT_SECONDS.get(test, TEST_TIMEOUT_SECONDS)
                    r = run([str(executable), *FIXTURE_ARGUMENTS.get(test, ())], cwd=executable.parent, timeout=limit)
                    why = first_failure_line(r.stdout.replace(str(source) + os.sep, "")) if r.returncode != 0 else ""
                    outcome[test] = (r.returncode != 0, why)
                except subprocess.TimeoutExpired:
                    outcome[test] = (True, f"timeout ({limit} s)")
                if stop_at_kill and outcome[test][0]:
                    break
            return outcome

        result, _ = build_tests()
        if result.returncode != 0:
            print(result.stdout[-4000:])
            print("FAIL  baseline build")
            return 1
        baseline = run_tests(fixtures)
        if any(failed for failed, _ in baseline.values()):
            print(f"FAIL  baseline: the unmutated fixtures must pass: {baseline}")
            return 1
        print(f"ok    baseline: {', '.join(fixtures)} pass unmutated")

        problems = []
        rows = []
        for m in selected:
            mutant_started = time.monotonic()
            path = source / m.path
            original = path.read_bytes()
            try:
                stamper.write(path, apply_edit(source_text(original), m).encode("utf-8"))
                built, relinked = build_tests(m.tests)
                if built.returncode != 0:
                    problems.append(f"{m.name}: mutant does not compile (fix the mutant; compilation failure is not a kill)")
                    print(built.stdout[-3000:])
                    rows.append((m, "NO-BUILD", ""))
                    continue
                if not relinked:
                    problems.append(f"{m.name}: stale build: no fixture executable of {', '.join(m.tests)} was relinked after "
                                    "the mutation was written")
                    rows.append((m, "STALE", ""))
                    print(f"{'STALE':12} {m.name:40} {m.path}: stale build, mutant not judged")
                    continue
                outcome = run_tests(m.tests, stop_at_kill=True)
            finally:
                stamper.write(path, original)
            killers = [f"{t}: {why}" for t, (failed, why) in outcome.items() if failed]
            if m.equivalent:
                status = "EQUIV-KILLED" if killers else "equivalent"
                if killers:
                    problems.append(f"{m.name}: listed equivalent but killed ({killers[0]}): reclassify it")
                rows.append((m, status, killers[0] if killers else m.equivalent))
            else:
                status = "killed" if killers else "SURVIVED"
                if not killers:
                    problems.append(f"{m.name}: survived every fixture test ({', '.join(m.tests)})")
                rows.append((m, status, killers[0] if killers else ""))
            print(f"{status:12} {m.name:40} {m.path}: {m.summary} [{time.monotonic() - mutant_started:.1f} s]")
            if killers or m.equivalent:
                print(f"{'':12} -> {rows[-1][2]}")
        # A final rebuild proves the copy was restored (the restored tree builds and passes again).
        final, relinked = build_tests(fixtures if selected else ())
        if final.returncode != 0 or any(failed for failed, _ in run_tests(fixtures).values()):
            problems.append("restored tree no longer builds/passes after the mutation loop")
        elif not relinked:
            problems.append("stale build: the restored tree was not rebuilt after the mutation loop")
        killed = sum(1 for _, status, _ in rows if status == "killed")
        equivalent = sum(1 for _, status, _ in rows if status == "equivalent")
        print(f"summary: {killed} killed, {equivalent} justified equivalent, {len(rows) - killed - equivalent} problem(s), "
              f"{time.monotonic() - started:.0f} s")
        if problems:
            print("\n".join("FAIL  " + p for p in problems))
            return 1
        print("analysis_mutation_test: OK")
        return 0
    finally:
        if args.keep:
            print(f"kept {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
