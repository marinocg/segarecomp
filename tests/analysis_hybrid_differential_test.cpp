// SEG-031 (ADR 0080): seeded randomized differential of the hybrid admission planner against the bounded concrete executor of the
// SEG-030-T008 differential (reused verbatim: its generator, interpreter and pinned cases; its own `main` is renamed). TEST-ONLY.
//
// Each generated image (project-authored encodings: field dispatch through `JSR (An)` and `JMP (d8,PC,Dn.W)`, object loops, calls and
// returns, push windows, return-slot rewrites, TRAP/RTE frames, SR mask changes, stores through known and Unknown bases, injected
// level-6 interrupts) is planned as a Genesis program from the 68000 reset state. Checks:
//   * containment: when the plan is hybrid, EVERY concretely executed PC lies in the hybrid admission H (the generated program could
//     execute nothing else): an observed PC outside H is a BLOCKER, never explained away (not observed proves nothing);
//   * hybrid ⊆ broad U; a broad outcome admits U exactly; the plan and its production artifact are deterministic;
//   * the island machinery is exercised: the run reports how many images were hybrid, how many had islands, and every broad outcome
//     class (whole_image is expected to dominate: most generated dispatches read Unknown state).
// Any escape prints the seed, the step and the listing (a minimizable reproducer).

#define main analysis_m68k_differential_main
#include "analysis_m68k_differential_test.cpp"
#undef main

#include "segarecomp/genesis_analysis_report/hybrid_plan.hpp"

namespace {

segarecomp::GenesisHybridPlan plan_image(const Image &image, std::optional<segarecomp::FrontendProgram> &program) {
  program = segarecomp::make_genesis_bridge_startup_program(image.bytes, 0U, main_entry, std::nullopt);
  if (!program || !segarecomp::apply_genesis_immutable_rom_aot(*program)) return {};
  segarecomp::GenesisHybridPlanConfig config{};
  config.analysis.reset_entry = true;  // the concrete executor starts from the 68000 reset state (S = 1, I = 7, SSP = vector 0)
  return segarecomp::plan_genesis_hybrid_admission(*program, config);
}

}  // namespace

int main(int argc, char **argv) {
  std::uint64_t base = UINT64_C(0x5E6031000);
  std::size_t count = 300U;
  if (argc > 1) count = static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10));
  if (argc > 2) base = std::strtoull(argv[2], nullptr, 0);
  const auto started = std::chrono::steady_clock::now();
  std::size_t images = 0U, rejected = 0U, failures = 0U, hybrid = 0U, with_islands = 0U, steps_checked = 0U;
  std::map<std::string, std::size_t> outcomes;
  for (std::size_t n = 0; n < count + 4U; ++n) {
    const bool pinned = n < 4U;
    const auto seed = pinned ? UINT64_C(0x5E60300F1) + n : base + n - 4U;
    const auto image = pinned ? (n < 2U ? pinned_resumption_case(n == 1U) : pinned_unproven_offset_case(n == 3U))
                              : Generator{seed}.build();
    if (image.bytes.empty()) {
      ++rejected;
      continue;
    }
    ++images;
    std::optional<segarecomp::FrontendProgram> program;
    const auto plan = plan_image(image, program);
    if (!program) {
      std::cerr << "FAIL: the image is not a Genesis bridge program, seed " << seed << '\n';
      ++failures;
      continue;
    }
    ++outcomes[segarecomp::genesis_hybrid_outcome_name(plan.outcome)];
    std::optional<segarecomp::FrontendProgram> again_program;
    const auto again = plan_image(image, again_program);
    const auto artifact = segarecomp::format_genesis_hybrid_admission_plan(segarecomp::genesis_hybrid_admission_plan(plan, *program, std::string(64U, 'a')));
    if (again.admitted != plan.admitted || again.outcome != plan.outcome ||
        artifact != segarecomp::format_genesis_hybrid_admission_plan(segarecomp::genesis_hybrid_admission_plan(again, *again_program, std::string(64U, 'a')))) {
      std::cerr << "FAIL: non-deterministic plan, seed " << seed << '\n';
      ++failures;
    }
    if (!std::includes(plan.universe.begin(), plan.universe.end(), plan.admitted.begin(), plan.admitted.end())) {
      std::cerr << "FAIL: hybrid admission is not a subset of broad U, seed " << seed << '\n';
      ++failures;
    }
    if (plan.outcome != segarecomp::GenesisHybridOutcome::hybrid) {
      if (plan.admitted != plan.universe) {
        std::cerr << "FAIL: a broad outcome does not admit U exactly, seed " << seed << '\n';
        ++failures;
      }
      continue;
    }
    ++hybrid;
    with_islands += plan.island_entries.empty() ? 0U : 1U;
    const std::set<std::uint32_t> admitted_h(plan.hybrid.begin(), plan.hybrid.end());
    for (const bool preserve : {false, true}) {
      const auto trace = Machine{image, seed ^ UINT64_C(0xA5A5A5A5), preserve}.run();
      for (std::size_t i = 0; i < trace.steps.size(); ++i) {
        ++steps_checked;
        const auto pc = trace.steps[i].pc & UINT32_C(0x00FFFFFF);
        if (admitted_h.contains(pc)) continue;
        ++failures;
        std::cerr << "BLOCKER (observed PC outside the hybrid admission) seed=" << seed << " step=" << i << " pc=" << std::hex << pc
                  << std::dec << " preserve=" << preserve << '\n'
                  << image.listing;
        break;
      }
    }
  }
  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::cout << "analysis_hybrid_differential_test: pinned=4 seeds " << base << ".." << base + count - 1U << " images=" << images
            << " rejected=" << rejected << " hybrid=" << hybrid << " with_islands=" << with_islands << " steps_checked=" << steps_checked
            << " seconds=" << elapsed << '\n';
  for (const auto &[outcome, number] : outcomes) std::cout << "  outcome " << outcome << '=' << number << '\n';
  if (hybrid == 0U) {
    std::cerr << "FAIL: no generated image produced a hybrid plan (the containment check was never exercised)\n";
    ++failures;
  }
  if (failures != 0U) {
    std::cerr << failures << " failure(s)\n";
    return 1;
  }
  return 0;
}
