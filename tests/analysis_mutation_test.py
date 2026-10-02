#!/usr/bin/env python3
"""SEG-029-T005 (ADR 0078): adversarial mutation gate for the generic abstract-analysis core and its CPU adapters.

Every mutant below is one precise textual edit of PRODUCTION analysis code (the generic core headers, the M68K adapter, the Z80
effect projection or the Z80 adapter) that plants a deliberate soundness, determinism or bound defect. The harness copies the
product tree (without build/, .git/, games/, .tools/) to a temporary directory, configures ONE build there, builds the three
SEG-029 fixture tests and requires the unmutated baseline to pass. Then, per mutant, it applies the edit to the temporary copy only,
rebuilds the affected fixture tests incrementally, runs them, and restores the file. A mutant is KILLED when one of its fixture tests
exits non-zero (or times out). The worktree is never modified.

Fail-closed rules:
  * stale mutant: the edit's `old` text must occur exactly once in the current source, otherwise the harness FAILS (a refactor can
    never silently disable a mutant; update the mutant together with the refactor);
  * a mutant that does not compile FAILS the harness (compilation failure is not a kill);
  * a surviving mutant FAILS the harness, unless it is listed as `equivalent` with a written justification, in which case it must
    still compile and must SURVIVE (a killed "equivalent" mutant means the justification is stale and FAILS the harness).
Self-checks: the baseline must pass all three tests, and the stale-edit detector must reject a missing and a duplicated pattern.

Equivalent mutants (justified, asserted to survive):
  * m68k_failed_read_ignored: `m68k_finite_register_after` (the CPU semantic owner) already returns a default, not-`known`
    `M68kFiniteValues` (Unknown) whenever `immutable_read_failed` is set, so the adapter's explicit override is defence in depth.
  * m68k_flag_setter_adjacency_unchecked: a state's `flag_setter` is only ever created on a fallthrough/branch edge whose target is
    the physically next instruction (`target == next`), joins keep only agreeing setters, and entries/continuations carry none, so
    the re-check of physical adjacency at the consumer can never fail.

usage: analysis_mutation_test.py <product-root> <cmake> <c-compiler> <cxx-compiler> [--generator G] [--make-program P]
                                 [--only NAME[,NAME...]] [--keep]
"""
from __future__ import annotations

import argparse
import dataclasses
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
ALL_TESTS = (CORE_TEST, Z80_TEST, M68K_TEST)  # cheapest first: a core mutant is usually decided by the CPU-free fixture
TEST_TIMEOUT_SECONDS = 60  # the unmutated fixtures run in well under a second

FINITE = "libs/analysis/include/segarecomp/analysis/finite_value.hpp"
SOLVER = "libs/analysis/include/segarecomp/analysis/solver.hpp"
M68K = "libs/cpu/m68k/analysis/src/finite_adapter.cpp"
Z80_EFFECTS = "libs/cpu/z80/src/effects.cpp"
Z80_ADAPTER = "libs/cpu/z80/analysis/src/adapter.cpp"


@dataclasses.dataclass(frozen=True)
class Mutant:
    name: str
    path: str
    old: str
    new: str
    tests: tuple[str, ...]
    summary: str
    equivalent: str = ""  # non-empty: written justification; the mutant must compile and survive


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
           "    const auto point = *worklist.begin();\n    worklist.erase(worklist.begin());",
           "    const auto point = *worklist.rbegin();\n    worklist.erase(std::prev(worklist.end()));",
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
           "    out.unresolved_computed.erase(point);\n    out.computed_targets.erase(point);\n",
           "",
           ALL_TESTS, "a computed site keeps an earlier transfer's resolution"),
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
    Mutant("m68k_odd_target_kept", M68K,
           "    if ((target & 1U) != 0U) {",
           "    if ((target & 1U) != 0U && false) {",
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
    Mutant("m68k_invalidation_skipped", M68K,
           "        invalidated.insert(pc);",
           "        {}",
           (M68K_TEST,), "a site whose emitted targets were lost is never pinned (no restart)"),
    Mutant("m68k_unresolved_unreported", M68K,
           "        result.unresolved_computed = report.reason;\n",
           "",
           (M68K_TEST,), "an unresolved PC-indexed site is not reported unresolved"),
    Mutant("m68k_call_continuation_keeps_state", M68K,
           "if (config_.call_continuations) result.edges.push_back({stacked, EdgeKind::return_edge, State::all_unknown()});",
           "if (config_.call_continuations) result.edges.push_back({stacked, EdgeKind::return_edge, out});",
           (M68K_TEST,), "a call continuation carries the pre-call state instead of an opaque entry"),
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
]


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
    names = [m.name for m in MUTANTS]
    if len(names) != len(set(names)):
        raise SystemExit("FAIL self-check: duplicate mutant names")
    for m in MUTANTS:
        if m.old == m.new or not set(m.tests) <= set(ALL_TESTS):
            raise SystemExit(f"FAIL self-check: malformed mutant {m.name}")
    print("ok    self-check: stale edit patterns (missing, duplicated) are rejected")


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
    parser.add_argument("--keep", action="store_true", help="keep the temporary build directory")
    args = parser.parse_args()
    root = args.product_root.resolve()
    started = time.monotonic()

    self_check_stale_detection()
    selected = MUTANTS
    if args.only:
        wanted = set(args.only.split(","))
        unknown = wanted - {m.name for m in MUTANTS}
        if unknown:
            raise SystemExit(f"FAIL: unknown mutant(s) {sorted(unknown)}")
        selected = [m for m in MUTANTS if m.name in wanted]
    # Stale detection against the real sources before any build: every edit must match exactly once.
    stale = []
    for m in selected:
        try:
            apply_edit((root / m.path).read_text(encoding="utf-8"), m)
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

        def build_tests(tests: tuple[str, ...]) -> subprocess.CompletedProcess:
            return run([args.cmake, "--build", str(build), "--config", "Debug", "--parallel", jobs, "--target", *tests])

        # Tests run in the listed order; with `stop_at_kill` the first failing test decides (a mutant can make a later fixture slow,
        # e.g. a join defect that oscillates until the solver's iteration bound). A timeout is an observable failure (a kill).
        def run_tests(tests: tuple[str, ...], stop_at_kill: bool = False) -> dict[str, tuple[bool, str]]:
            outcome = {}
            for test in tests:
                executable = find_executable(build, test)
                try:
                    r = run([str(executable)], cwd=executable.parent, timeout=TEST_TIMEOUT_SECONDS)
                    why = first_failure_line(r.stdout.replace(str(source) + os.sep, "")) if r.returncode != 0 else ""
                    outcome[test] = (r.returncode != 0, why)
                except subprocess.TimeoutExpired:
                    outcome[test] = (True, f"timeout ({TEST_TIMEOUT_SECONDS} s)")
                if stop_at_kill and outcome[test][0]:
                    break
            return outcome

        result = build_tests(ALL_TESTS)
        if result.returncode != 0:
            print(result.stdout[-4000:])
            print("FAIL  baseline build")
            return 1
        baseline = run_tests(ALL_TESTS)
        if any(failed for failed, _ in baseline.values()):
            print(f"FAIL  baseline: the unmutated fixtures must pass: {baseline}")
            return 1
        print(f"ok    baseline: {', '.join(ALL_TESTS)} pass unmutated")

        problems = []
        rows = []
        for m in selected:
            mutant_started = time.monotonic()
            path = source / m.path
            original = path.read_bytes()
            try:
                path.write_text(apply_edit(original.decode("utf-8"), m), encoding="utf-8", newline="")
                built = build_tests(m.tests)
                if built.returncode != 0:
                    problems.append(f"{m.name}: mutant does not compile (fix the mutant; compilation failure is not a kill)")
                    print(built.stdout[-3000:])
                    rows.append((m, "NO-BUILD", ""))
                    continue
                outcome = run_tests(m.tests, stop_at_kill=True)
            finally:
                path.write_bytes(original)
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
        if build_tests(ALL_TESTS).returncode != 0 or any(failed for failed, _ in run_tests(ALL_TESTS).values()):
            problems.append("restored tree no longer builds/passes after the mutation loop")
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
