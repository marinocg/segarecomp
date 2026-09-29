// SEG-026-T001: complete execution-PC coverage (runtime observer + headless frame-bounded run).
// Project-authored synthetic runtime state only; no ROM, no host clock.
#include "execution_coverage.h"
#include "runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool value, const char *message) {
  if (!value) {
    std::printf("FAIL: %s\n", message);
    ++failures;
  }
}

// Synthetic program (one dispatch function standing in for generated code, same retirement contract):
//   0x200..0x204: a three-instruction ordinary block retired within ONE dispatch; then a transfer to 0x210.
//   0x210: alternates between 0x200 and 0x220.   0x220: back to 0x200.
//   0x300: IRQ6 handler standing in for RTE: pops the basic frame (SR, PC) and resumes.
// Every instruction retires 4 cycles (the handler 20).
GenesisControlTransfer block(GenesisRuntime *r, std::uint32_t first, unsigned count, std::uint32_t exit_pc) {
  GenesisControlTransfer retired{};
  for (unsigned i = 0; i < count; ++i) {
    const std::uint32_t next = i + 1U == count ? exit_pc : first + 2U * (i + 1U);
    r->pc = next;
    retired = genesis_runtime_retire_m68k_instruction(r, 4U, r->pc);
    if (retired.kind != GENESIS_CONTINUE_AT_PC || retired.next_pc != next) return retired;
  }
  return retired;
}

GenesisControlTransfer program(GenesisRuntime *r) {
  switch (r->pc) {
  case 0x200U: return block(r, 0x200U, 3U, 0x210U);
  case 0x202U: return block(r, 0x202U, 2U, 0x210U);  // resumption entry into the block
  case 0x204U: return block(r, 0x204U, 1U, 0x210U);
  case 0x210U:
    r->d[0] += 1U;
    r->pc = (r->d[0] & 1U) != 0U ? 0x200U : 0x220U;
    return genesis_runtime_retire_m68k_instruction(r, 4U, r->pc);
  case 0x220U:
    r->pc = 0x200U;
    return genesis_runtime_retire_m68k_instruction(r, 4U, r->pc);
  case 0x300U: {
    const std::uint32_t sp = r->a[7] & 0xFFFFU;
    r->sr = static_cast<std::uint16_t>((r->work_ram[sp] << 8) | r->work_ram[sp + 1U]);
    r->pc = (static_cast<std::uint32_t>(r->work_ram[sp + 2U]) << 24) | (static_cast<std::uint32_t>(r->work_ram[sp + 3U]) << 16) |
            (static_cast<std::uint32_t>(r->work_ram[sp + 4U]) << 8) | r->work_ram[sp + 5U];
    r->a[7] += 6U;
    return genesis_runtime_retire_m68k_instruction(r, 20U, r->pc);
  }
  default: {
    GenesisControlTransfer t{};
    t.kind = GENESIS_STOP;
    t.stop.stop_class = GENESIS_STOP_KNOWN_BUT_UNEMITTED_TARGET;
    return t;
  }
  }
}

void setup(GenesisRuntime &r, bool interrupts) {
  std::memset(static_cast<void *>(&r), 0, sizeof(r));
  r.pc = 0x200U;
  r.sr = interrupts ? 0x2000U : 0x2700U;
  r.a[7] = 0x00FFFF00U;
  std::uint16_t *regs = r.devices.vdp.registers;
  regs[1] = interrupts ? 0x64 : 0x44; regs[2] = 0x30; regs[4] = 0x04; regs[5] = 0x28; regs[11] = 0x04;
  regs[12] = 0x81; regs[16] = 0x01;
  r.irq6_handler_entry = 0x300U;
  r.irq6_handler_present = interrupts ? 1U : 0U;
  // Place virtual time so that the SECOND retirement of the first block crosses the VBlank onset: the
  // interrupt then replaces 0x204 (never executed before) as that retirement's successor.
  r.scheduler.master_ticks = GENESIS_NTSC_VBLANK_ONSET_TICK - 7U * 4U * 2U + 1U;
}

struct Recorded {
  std::vector<std::uint8_t> bitmap = std::vector<std::uint8_t>(GENESIS_EXECUTION_COVERAGE_BITMAP_BYTES);
  std::vector<GenesisExecutionCoverageWitness> witnesses = std::vector<GenesisExecutionCoverageWitness>(64);
  GenesisExecutionCoverage coverage{};
  Recorded() {
    coverage.bitmap = bitmap.data();
    coverage.witnesses = witnesses.data();
    coverage.witness_capacity = witnesses.size();
  }
};

void run_steps(GenesisRuntime &r, unsigned steps) {
  for (unsigned i = 0; i < steps; ++i) {
    const auto t = genesis_runtime_step(&r, program);
    if (t.kind != GENESIS_CONTINUE_AT_PC) break;
  }
}

const GenesisExecutionCoverageWitness *witness_for(const Recorded &rec, std::uint32_t pc) {
  for (std::uint64_t i = 0; i < rec.coverage.witness_count; ++i)
    if (rec.witnesses[i].pc == pc) return &rec.witnesses[i];
  return nullptr;
}

// Fixture 13a: the exact retired-PC set, including interior block instructions, and zero semantic effect.
void exact_set_and_zero_effect() {
  GenesisRuntime plain, observed;
  setup(plain, false);
  setup(observed, false);
  Recorded rec;
  observed.execution_coverage = &rec.coverage;
  run_steps(plain, 40U);
  run_steps(observed, 40U);
  observed.execution_coverage = nullptr;
  check(std::memcmp(&plain, &observed, sizeof(GenesisRuntime)) == 0, "coverage has zero semantic effect");
  const std::set<std::uint32_t> expected{0x200U, 0x202U, 0x204U, 0x210U, 0x220U};
  std::set<std::uint32_t> got;
  for (std::uint32_t pc = 0; pc < 0x1000U; pc += 2U)
    if (genesis_execution_coverage_contains(&rec.coverage, pc)) got.insert(pc);
  check(got == expected, "bitmap holds exactly the retired PCs (interior block PCs included)");
  check(rec.coverage.distinct_count == expected.size(), "distinct count");
  check(rec.coverage.witness_count == expected.size() && rec.coverage.witness_overflow == 0U, "one witness per PC");
  check(rec.coverage.unknown_retirement_count == 0U && rec.coverage.odd_pc_count == 0U, "no unknown/odd retirements");
  check(!genesis_execution_coverage_contains(&rec.coverage, 0x206U), "a never-retired PC is absent");
  // Fixture 14: first-entry witnesses.
  const auto *w200 = witness_for(rec, 0x200U);
  const auto *w202 = witness_for(rec, 0x202U);
  const auto *w210 = witness_for(rec, 0x210U);
  const auto *w220 = witness_for(rec, 0x220U);
  check(w200 != nullptr && w200->cause == GENESIS_COVERAGE_CAUSE_INITIAL && w200->retirement_ordinal == 0U, "initial witness");
  check(w202 != nullptr && w202->previous_pc == 0x200U && w202->cause == GENESIS_COVERAGE_CAUSE_RETIRE_SUCCESSOR,
        "in-block successor witness");
  check(w210 != nullptr && w210->previous_pc == 0x204U, "block exit witness");
  check(w220 != nullptr && w220->previous_pc == 0x210U && w220->cause == GENESIS_COVERAGE_CAUSE_RETIRE_SUCCESSOR,
        "transfer witness");
  // Determinism: an identical second run yields the identical bitmap.
  GenesisRuntime again;
  setup(again, false);
  Recorded rec2;
  again.execution_coverage = &rec2.coverage;
  run_steps(again, 40U);
  check(rec.bitmap == rec2.bitmap && rec.coverage.retirement_count == rec2.coverage.retirement_count,
        "deterministic coverage");
}

// Fixture 13b/14: interrupt redirect and resumption attribution.
void interrupt_redirect_and_resumption() {
  GenesisRuntime plain, observed;
  setup(plain, true);
  setup(observed, true);
  Recorded rec;
  observed.execution_coverage = &rec.coverage;
  run_steps(plain, 12U);
  run_steps(observed, 12U);
  observed.execution_coverage = nullptr;
  check(std::memcmp(&plain, &observed, sizeof(GenesisRuntime)) == 0, "zero semantic effect with interrupts");
  check(rec.coverage.interrupt_redirects >= 1U, "the VBlank interrupt redirected a successor");
  check(rec.coverage.interrupt_resumptions >= 1U, "the handler resumed the interrupted successor");
  const auto *handler = witness_for(rec, 0x300U);
  check(handler != nullptr && handler->cause == GENESIS_COVERAGE_CAUSE_INTERRUPT_ENTRY && handler->previous_pc == 0x202U,
        "handler first entry is an interrupt entry after the interrupted retirement");
  const auto *resumed = witness_for(rec, 0x204U);
  check(resumed != nullptr && resumed->cause == GENESIS_COVERAGE_CAUSE_INTERRUPT_RESUMPTION && resumed->previous_pc == 0x202U,
        "interrupted first execution is attributed to its architectural predecessor, not the handler");
  check(resumed != nullptr && handler != nullptr && resumed->retirement_ordinal > handler->retirement_ordinal,
        "the resumed instruction first retires after the handler");
}

std::vector<char> read_file(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Fixture 13c: the frame-bounded host run: identical guest results with coverage on/off; deterministic digest.
void frame_bounded_run() {
  const auto dir = std::filesystem::temp_directory_path() / "segarecomp-t026-coverage";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto dir_text = dir.string();
  GenesisExecutionCoverageOptions options{};
  options.target_frames = 3U;
  options.epoch_frames = 1U;
  options.witness_capacity = 64U;
  options.private_dir = dir_text.c_str();
  options.coverage_enabled = 1;
  static GenesisExecutionCoverageResult on1, on2, off;
  GenesisRuntime r1, r2, r3;
  setup(r1, true);
  setup(r2, true);
  setup(r3, true);
  genesis_execution_coverage_run(&r1, program, &options, 100000000U, &on1);
  const auto bitmap = read_file(dir / "coverage.bitmap");
  const auto witnesses = read_file(dir / "witnesses.txt");
  genesis_execution_coverage_run(&r2, program, &options, 100000000U, &on2);
  options.coverage_enabled = 0;
  options.private_dir = nullptr;
  genesis_execution_coverage_run(&r3, program, &options, 100000000U, &off);
  check(on1.outcome == GENESIS_EXECUTION_COVERAGE_FRAMES_REACHED && off.outcome == GENESIS_EXECUTION_COVERAGE_FRAMES_REACHED,
        "frame target reached");
  check(on1.frames_published == 3U && on1.epoch_count == 3U, "three frames, three epochs");
  check(std::memcmp(on1.final_state_digest, off.final_state_digest, 32U) == 0 &&
            std::memcmp(on1.frame_stream_digest, off.frame_stream_digest, 32U) == 0 && on1.dispatches == off.dispatches,
        "coverage on/off: identical guest state, frames and dispatches");
  check(std::memcmp(on1.coverage_digest, on2.coverage_digest, 32U) == 0 &&
            on1.coverage.distinct_count == on2.coverage.distinct_count,
        "coverage digest is deterministic");
  check(bitmap.size() == GENESIS_EXECUTION_COVERAGE_BITMAP_BYTES && !witnesses.empty(), "private artifacts written");
  // Counting frame boundaries without rendering changes nothing observable in the guest.
  static GenesisExecutionCoverageResult norender;
  GenesisRuntime r4;
  setup(r4, true);
  options.skip_render = 1;
  genesis_execution_coverage_run(&r4, program, &options, 100000000U, &norender);
  options.skip_render = 0;
  check(norender.outcome == GENESIS_EXECUTION_COVERAGE_FRAMES_REACHED && norender.frames_published == 3U &&
            norender.dispatches == off.dispatches && std::memcmp(norender.final_state_digest, off.final_state_digest, 32U) == 0,
        "no-render counting: identical dispatches and final guest state");
  check(on1.coverage.distinct_count == 6U, "block PCs plus the handler");
  check(r1.execution_coverage == nullptr && r1.live_frame_observer == nullptr, "observers detached");
  GenesisRuntime busy;
  setup(busy, true);
  busy.execution_coverage = &on1.coverage;
  static GenesisExecutionCoverageResult rejected;
  genesis_execution_coverage_run(&busy, program, &options, 10U, &rejected);
  check(rejected.outcome == GENESIS_EXECUTION_COVERAGE_INVALID_ARGUMENT, "an attached observer is rejected");
  std::filesystem::remove_all(dir);
}
}  // namespace

int main() {
  exact_set_and_zero_effect();
  interrupt_redirect_and_resumption();
  frame_bounded_run();
  if (failures != 0) return 1;
  std::printf("genesis_execution_coverage_tests: OK\n");
  return 0;
}
