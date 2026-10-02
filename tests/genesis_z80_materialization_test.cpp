// SEG-032-T008 (ADR 0073): the build-time Z80 image materialization fixed point against scripted pass runners.
//
// The loop in platforms/genesis/machine/src/z80_materialization.cpp is pure platform logic: it decides from the ordered
// observations of bounded headless runs which images exist, when the set is complete, and which typed failure ends a build that
// cannot converge. The scripted runner below plays the part of `segarecomp build`'s process runner: it answers prepare() and
// run() from a fixed script and records what the loop asked for. The real compile-and-run pipeline is covered by
// tests/genesis_z80_build_pipeline_test.py.
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>

#include "segarecomp/machine/genesis/z80_materialization.hpp"

namespace z80 = segarecomp::machine::genesis::z80;

namespace {

int failures = 0;

void check(bool ok, const char* label) {
  std::printf("%-5s %s\n", ok ? "ok" : "FAIL", label);
  if (!ok) ++failures;
}

// A snapshot whose hold-window-written extent is `length` bytes of a pattern seeded by `seed` at offset 0.
z80::Epoch snapshot(unsigned seed, std::size_t length = 64) {
  z80::Epoch epoch;
  for (std::size_t i = 0; i < z80::kRamBytes; ++i) epoch.ram[i] = static_cast<std::uint8_t>((i * 31 + seed * 7 + 1) & 0xFF);
  for (std::size_t i = 0; i < length; ++i) epoch.written[i >> 3] = static_cast<std::uint8_t>(epoch.written[i >> 3] | (1U << (i & 7)));
  return epoch;
}

z80::PassEpoch pass_epoch(const z80::Epoch& epoch) {
  z80::PassEpoch out;
  out.signature = z80::signature_digest(epoch);
  out.extents = static_cast<std::uint32_t>(z80::extents(epoch.written).size());
  return out;
}

z80::PassObservation unknown_run(std::vector<z80::PassEpoch> known, const z80::Epoch& snap) {
  z80::PassObservation obs;
  obs.outcome = z80::PassOutcome::unknown_image;
  obs.epochs = std::move(known);
  obs.epochs.push_back(pass_epoch(snap));
  obs.unknown = snap;
  return obs;
}

z80::PassObservation done_run(std::vector<z80::PassEpoch> epochs, z80::PassOutcome outcome = z80::PassOutcome::window_complete,
                              std::uint32_t frames = z80::kObservationFrames) {
  z80::PassObservation obs;
  obs.outcome = outcome;
  obs.frames = frames;
  obs.epochs = std::move(epochs);
  return obs;
}

class Scripted final : public z80::PassRunner {
 public:
  bool prepare(const z80::Registry& registry) override {
    prepared_sizes.push_back(registry.images().size());
    return compile_ok;
  }
  z80::PrepareFailure prepare_failure() const override { return failure; }
  z80::PrepareFailure failure;
  z80::PassObservation run() override {
    ++runs;
    if (script.empty()) return {};  // outcome failed
    auto next = std::move(script.front());
    script.pop_front();
    return next;
  }
  std::deque<z80::PassObservation> script;
  std::vector<std::size_t> prepared_sizes;
  std::size_t runs = 0;
  bool compile_ok = true;
};

z80::MaterializationSummary run_script(Scripted& runner, z80::Registry& registry) { return z80::materialize(registry, runner); }

}  // namespace

int main() {
  static_assert(z80::kObservationFrames >= 600);
  static_assert(z80::kMaxDiscoveryRuns == z80::kMaxImages + 1);
  static_assert(z80::kMaxImages >= 4 && z80::kMaxImages <= z80::kMaxImagesCeiling);

  {  // a program with no Z80 epoch: converged at once, the confirming run is the second run
    Scripted r;
    r.script = {done_run({}), done_run({})};
    z80::Registry registry;
    const auto s = run_script(r, registry);
    check(s.ok() && s.images == 0 && s.discovery_runs == 1 && s.total_runs == 2 && r.runs == 2, "no epoch: converges with 0 images in 2 runs");
    check(r.prepared_sizes.size() == 1, "no epoch: the (empty) registry is prepared exactly once, the confirming run reuses it");
  }
  {  // multi-epoch: two distinct images, a repeat of the first signature and a restart; ordered by first activation
    const z80::Epoch a = snapshot(1), b = snapshot(2);
    Scripted r;
    const auto ea = pass_epoch(a), eb = pass_epoch(b);
    r.script = {unknown_run({}, a), unknown_run({ea}, b), done_run({ea, eb, ea}), done_run({ea, eb, ea})};
    z80::Registry registry;
    const auto s = run_script(r, registry);
    check(s.ok() && s.images == 2 && s.discovery_runs == 3 && s.total_runs == 4 && s.epochs == 3, "multi epoch: exactly 2 images, 3 discovery runs, 1 confirming run");
    check(r.prepared_sizes == std::vector<std::size_t>({0, 1, 2}), "multi epoch: the registry grows by one image per rebuild (0, 1, 2)");
    check(registry.images().size() == 2 && registry.images()[0].signature == ea.signature && registry.images()[1].signature == eb.signature &&
              registry.images()[0].ordinal == 1 && registry.images()[1].ordinal == 2,
          "multi epoch: images are ordered by first activation");
    check(s.end == z80::PassOutcome::window_complete && s.frames == z80::kObservationFrames, "multi epoch: the window was fully observed");
  }
  {  // a guest that stops (non-Z80) inside the window is a candidate too
    const z80::Epoch a = snapshot(1);
    const auto ea = pass_epoch(a);
    Scripted r;
    r.script = {unknown_run({}, a), done_run({ea}, z80::PassOutcome::guest_stop, 54), done_run({ea}, z80::PassOutcome::guest_stop, 54)};
    z80::Registry registry;
    const auto s = run_script(r, registry);
    check(s.ok() && s.end == z80::PassOutcome::guest_stop && s.frames == 54, "guest stop inside the window: converged, frames reached recorded");
  }
  {  // structural code mutation: the sound CPU is isolated; the build converges degraded and discovery stops
    const z80::Epoch a = snapshot(1);
    const auto ea = pass_epoch(a);
    const auto faulted = [&](std::uint32_t epochs, std::uint64_t ticks) {
      z80::PassObservation obs = done_run({ea});
      obs.sound_fault = z80::SoundFault{epochs, ticks};
      return obs;
    };
    Scripted r;
    r.script = {unknown_run({}, a), faulted(1, 1000), faulted(1, 1000)};
    z80::Registry registry;
    const auto s = run_script(r, registry);
    check(s.ok() && s.sound_fault && s.sound_fault->epochs == 1 && s.sound_fault->master_ticks == 1000 && s.images == 1,
          "a fault-isolated run converges degraded with the images known so far");
    Scripted healthy;
    healthy.script = {unknown_run({}, a), done_run({ea}), done_run({ea})};
    z80::Registry registry_healthy;
    const auto h = run_script(healthy, registry_healthy);
    check(h.ok() && !h.sound_fault, "a fault-free run is not degraded");
    Scripted r2;
    r2.script = {unknown_run({}, a), faulted(1, 1000), faulted(1, 1001)};
    z80::Registry registry2;
    check(run_script(r2, registry2).failure == z80::Failure::materialization_nondeterministic, "a confirming run faulting at a different time: materialization_nondeterministic");
    Scripted r3;
    r3.script = {unknown_run({}, a), faulted(1, 1000), done_run({ea})};
    z80::Registry registry3;
    check(run_script(r3, registry3).failure == z80::Failure::materialization_nondeterministic, "a confirming run that does not fault: materialization_nondeterministic");
    Scripted r4;
    r4.script = {unknown_run({}, a), done_run({ea}), faulted(1, 1000)};
    z80::Registry registry4;
    check(run_script(r4, registry4).failure == z80::Failure::materialization_nondeterministic, "a confirming run that newly faults: materialization_nondeterministic");
  }
  {  // nondeterminism: the confirming run differs
    const z80::Epoch a = snapshot(1), b = snapshot(2);
    const auto ea = pass_epoch(a), eb = pass_epoch(b);
    Scripted r;
    r.script = {unknown_run({}, a), done_run({ea}), done_run({ea, eb})};
    z80::Registry registry;
    check(run_script(r, registry).failure == z80::Failure::materialization_nondeterministic, "confirming run with an extra epoch: materialization_nondeterministic");
    Scripted r2;
    r2.script = {unknown_run({}, a), done_run({ea}), done_run({ea}, z80::PassOutcome::window_complete, z80::kObservationFrames - 1)};
    z80::Registry registry2;
    check(run_script(r2, registry2).failure == z80::Failure::materialization_nondeterministic, "confirming run with a different frame count: materialization_nondeterministic");
    Scripted r3;
    r3.script = {unknown_run({}, a), done_run({ea}), done_run({ea}, z80::PassOutcome::guest_stop)};
    z80::Registry registry3;
    check(run_script(r3, registry3).failure == z80::Failure::materialization_nondeterministic, "confirming run ending differently: materialization_nondeterministic");
    Scripted r4;
    r4.script = {unknown_run({}, a), done_run({ea}), unknown_run({ea}, b)};
    z80::Registry registry4;
    check(run_script(r4, registry4).failure == z80::Failure::materialization_nondeterministic, "confirming run finding a new unknown image: materialization_nondeterministic");
    Scripted r5;
    r5.script = {unknown_run({}, a), done_run({ea}), done_run({ea}, z80::PassOutcome::budget_exhausted)};
    z80::Registry registry5;
    check(run_script(r5, registry5).failure == z80::Failure::materialization_nondeterministic, "confirming run exhausting its budget: materialization_nondeterministic");
    // The previous run's epochs must recur as a prefix of the next run
    Scripted r6;
    r6.script = {unknown_run({}, a), unknown_run({pass_epoch(snapshot(9))}, b)};
    z80::Registry registry6;
    check(run_script(r6, registry6).failure == z80::Failure::materialization_nondeterministic, "a run that does not reproduce the previous epochs: materialization_nondeterministic");
  }
  {  // bounds
    Scripted r;
    std::vector<z80::PassEpoch> known;
    for (std::size_t k = 0; k <= z80::kMaxImages; ++k) {
      const z80::Epoch e = snapshot(static_cast<unsigned>(10 + k));
      r.script.push_back(unknown_run(known, e));
      known.push_back(pass_epoch(e));
    }
    z80::Registry registry;
    const auto s = run_script(r, registry);
    check(s.failure == z80::Failure::z80_image_bound_exceeded && registry.images().size() == z80::kMaxImages,
          "image bound + 1 distinct images: z80_image_bound_exceeded with exactly the bound registered");
    Scripted ok;
    known.clear();
    for (std::size_t k = 0; k < z80::kMaxImages; ++k) {
      const z80::Epoch e = snapshot(static_cast<unsigned>(10 + k));
      ok.script.push_back(unknown_run(known, e));
      known.push_back(pass_epoch(e));
    }
    ok.script.push_back(done_run(known));
    ok.script.push_back(done_run(known));
    z80::Registry full;
    const auto t = run_script(ok, full);
    check(t.ok() && t.images == z80::kMaxImages && t.discovery_runs == z80::kMaxDiscoveryRuns, "exactly the image bound converges within bound + 1 discovery runs");
  }
  {  // terminal outcomes of the pass
    struct Case { z80::PassOutcome outcome; z80::Failure expect; const char* label; } cases[] = {
        {z80::PassOutcome::budget_exhausted, z80::Failure::materialization_budget_exhausted, "instruction budget exhausted: materialization_budget_exhausted"},
        {z80::PassOutcome::wall_timeout, z80::Failure::materialization_budget_exhausted, "wall timeout: materialization_budget_exhausted"},
        {z80::PassOutcome::z80_code_mismatch, z80::Failure::z80_code_mismatch, "live code differs from the compiled image: z80_code_mismatch"},
        {z80::PassOutcome::z80_stop, z80::Failure::z80_execution_unsupported, "any other typed Z80 stop: z80_execution_unsupported"},
        {z80::PassOutcome::failed, z80::Failure::materialization_pass_failed, "no usable pass report: materialization_pass_failed"},
    };
    for (const auto& c : cases) {
      Scripted r;
      z80::PassObservation obs;
      obs.outcome = c.outcome;
      r.script = {obs};
      z80::Registry registry;
      check(run_script(r, registry).failure == c.expect, c.label);
    }
  }
  {  // build failures and internal-consistency failures
    Scripted r;
    r.compile_ok = false;
    z80::Registry registry;
    check(run_script(r, registry).failure == z80::Failure::z80_image_compile_failed && r.runs == 0, "image compile failure: z80_image_compile_failed, nothing is run");
    Scripted staged;
    staged.compile_ok = false;
    staged.failure = {"unit-compile", "synthetic diagnostic"};
    z80::Registry registry1;
    const auto staged_summary = run_script(staged, registry1);
    check(staged_summary.failure == z80::Failure::z80_image_compile_failed && staged_summary.prepare_failure.stage == "unit-compile" &&
              staged_summary.prepare_failure.detail == "synthetic diagnostic",
          "prepare failure: the typed failure is unchanged and the failed stage/detail are carried");
    check(run_script(r, registry).prepare_failure.stage.empty(), "a runner without stage detail reports empty strings");
    const z80::Epoch a = snapshot(1);
    Scripted dup;
    dup.script = {unknown_run({}, a), unknown_run({pass_epoch(a)}, a)};  // the pass claims the registered image is unknown
    z80::Registry registry2;
    check(run_script(dup, registry2).failure == z80::Failure::materialization_pass_failed, "an already registered image reported unknown: materialization_pass_failed");
    Scripted wrong;
    auto obs = unknown_run({}, a);
    obs.epochs.back().signature[0] ^= 0x5A;  // the program's signature does not match the build's derivation
    wrong.script = {obs};
    z80::Registry registry3;
    check(run_script(wrong, registry3).failure == z80::Failure::materialization_pass_failed, "pass and build disagree on the activation signature: materialization_pass_failed");
    Scripted nosnap;
    auto missing = unknown_run({}, a);
    missing.unknown.reset();
    nosnap.script = {missing};
    z80::Registry registry4;
    check(run_script(nosnap, registry4).failure == z80::Failure::materialization_pass_failed, "unknown image without a snapshot: materialization_pass_failed");
  }
  {  // a snapshot that differs only in carry-over data (outside the written extent) is the same image
    z80::Epoch a = snapshot(1), a2 = snapshot(1);
    a2.ram[4000] ^= 0xFF;
    check(z80::signature_digest(a) == z80::signature_digest(a2) && z80::content_digest(a.ram) != z80::content_digest(a2.ram),
          "the signature ignores carry-over data, the content hash does not");
  }
  std::printf("genesis z80 materialization: %s\n", failures == 0 ? "ok" : "FAILED");
  return failures == 0 ? 0 : 1;
}
