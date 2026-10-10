// SEG-007-T047 / ADR-0020: focused unit coverage of the deterministic VBlank /
// MC68000 IRQ6 interrupt mechanism (now inside genesis_runtime_step, driven
// repeatedly by genesis_runtime_run) plus the RTE restoration helper
// genesis_exception_return. These drive the runtime directly with synthetic
// dispatch closures exactly as generated block C would; no target opcode,
// image, or decoder is involved. All fixtures are synthetic.
//
// SEG-007-T252 / ADR-0040: the former watchdog-progress-credit tests (IRQ6
// admission as a credit source, mixed loop/data/IRQ credit accounting) have
// been removed along with the generated-runtime progress watchdog itself --
// IRQ6/VBlank scheduler admission/arbitration/exception-frame/RTE/post-RTE
// grace semantics below are otherwise byte-for-byte unchanged.

#include "runtime.h"

#include <cstring>
#include <cstdint>
#include <cstdio>
#include <functional>

namespace {

int failures = 0;

void check(bool ok, const char *what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

std::function<GenesisControlTransfer(GenesisRuntime *)> g_step;
GenesisControlTransfer trampoline(GenesisRuntime *runtime) {
  GenesisControlTransfer result = g_step(runtime);
  if (result.kind == GENESIS_CONTINUE_AT_PC)
    return genesis_runtime_retire_m68k_instruction(runtime, 4u, result.next_pc);
  return result;
}

GenesisControlTransfer continue_at(uint32_t pc) {
  GenesisControlTransfer t{};
  t.kind = GENESIS_CONTINUE_AT_PC;
  t.next_pc = pc;
  /* Synthetic dispatches explicitly model one complete guest instruction. */
  return t;
}

constexpr uint32_t kRamBegin = 0x00FF0000u;
constexpr uint32_t kHandler = 0x00001000u;
constexpr uint32_t kOrdinaryPc = 0x00000200u;

// A runtime primed for admission: supervisor mode, mask open, IE0 enabled,
// stack well inside work RAM, a build-resolved IRQ6 handler.
void prime(GenesisRuntime &r) {
  r = GenesisRuntime{};
  r.a[7] = kRamBegin + 0x8000u;
  r.pc = kOrdinaryPc;
  r.sr = 0x2000u;  // S = 1, interrupt mask = 0, T = 0
  r.devices.vdp.registers[1] = 0x0020u;  // IE0 (bit 5) set
  r.irq6_handler_entry = kHandler;
  r.irq6_handler_present = 1u;
  r.scheduler.master_ticks = GENESIS_NTSC_VBLANK_ONSET_TICK - 28u;
}

uint8_t ram_byte(const GenesisRuntime &r, uint32_t addr) {
  return r.work_ram[addr - kRamBegin];
}

// ---------------------------------------------------------------------------
// Item 1: recurring, program-independent VBlank onset; exactly-once rising edge
// per period; IE0 gating; re-arm permits a later event.
void scheduler_recurs_and_gates() {
  GenesisRuntime r;
  prime(r);
  // Handler immediately RTEs; ordinary code spins forever otherwise.
  int admissions = 0;
  g_step = [&](GenesisRuntime *rt) -> GenesisControlTransfer {
    if (rt->pc == kHandler) {
      ++admissions;
      uint32_t restored = 0;
      GenesisRuntimeStop s{};
      check(genesis_exception_return(rt, &restored, &s) == 1, "handler RTE succeeds");
      return continue_at(restored);
    }
    return continue_at(kOrdinaryPc);
  };
  // Window larger than the period so the run survives to accumulate several
  // admissions, then reaches the watchdog once credit-gated progress lapses is
  // not the point here -- bound the observation with a generous window and a
  // finite dispatch via a step counter.
  int steps = 0;
  g_step = [&](GenesisRuntime *rt) -> GenesisControlTransfer {
    if (++steps > 20) {
      GenesisControlTransfer t{};
      t.kind = GENESIS_COMPLETE;
      return t;
    }
    if (rt->pc == kHandler) {
      ++admissions;
      uint32_t restored = 0;
      GenesisRuntimeStop s{};
      check(genesis_exception_return(rt, &restored, &s) == 1, "handler RTE succeeds");
      return continue_at(restored);
    }
    return continue_at(kOrdinaryPc);
  };
  const auto result = genesis_runtime_run(&r, trampoline, 4096u);
  check(result.kind == GENESIS_COMPLETE, "bounded synthetic run completes");
  check(admissions >= 1, "VBlank onset admits IRQ6 at its virtual-time boundary");
  // Exactly one rising edge per period: the count tracks admitted handler
  // entries to within the single in-flight period at the synthetic cap.
  check(r.devices.interrupt.vblank_transition_count >= (uint32_t)admissions &&
            r.devices.interrupt.vblank_transition_count <= (uint32_t)admissions + 1u,
        "exactly one rising edge per admitted period");

  // IE0 disabled: no rising edge ever, ordinary spin exhausts the runner's
  // own finite dispatch allowance -- SEG-007-T252 / ADR-0040: this is now the
  // disjoint GENESIS_RUNNER_RESOURCE_LIMIT outcome, never a guest semantic
  // stop (there is no watchdog left to fail closed).
  GenesisRuntime g;
  prime(g);
  g.devices.vdp.registers[1] = 0x0000u;  // IE0 clear
  g_step = [&](GenesisRuntime *rt) -> GenesisControlTransfer {
    (void)rt;
    return continue_at(kOrdinaryPc);
  };
  const auto gated = genesis_runtime_run(&g, trampoline, 64u);
  check(gated.kind == GENESIS_RUNNER_RESOURCE_LIMIT,
        "IE0 disabled: no interrupt, the runner's own allowance is exhausted instead");
  check(g.devices.interrupt.vblank_pending == 0u && g.devices.interrupt.vblank_transition_count == 0u,
        "IE0 disabled: pending/transition untouched");
}

// ---------------------------------------------------------------------------
// Item 2: masked IRQ6 stays pending and cannot fake runner progress; an
// eligible one is admitted; nothing eligible => ordinary dispatch.
// SEG-007-T252 / ADR-0040: renamed from masked_irq_cannot_suppress_watchdog
// -- there is no watchdog left, only the runner's own finite allowance.
void masked_irq_stays_pending_under_runner_resource_limit() {
  GenesisRuntime r;
  prime(r);
  r.sr = (uint16_t)(0x2000u | 0x0700u);  // S=1, mask = 7 (blocks level 6)
  g_step = [&](GenesisRuntime *rt) -> GenesisControlTransfer {
    (void)rt;
    return continue_at(kOrdinaryPc);
  };
  const auto result = genesis_runtime_run(&r, trampoline, 64u);
  check(result.kind == GENESIS_RUNNER_RESOURCE_LIMIT,
        "masked pending IRQ6 does not fake progress; the runner's own allowance is exhausted");
  check(r.devices.interrupt.vblank_pending == 1u, "masked IRQ6 remains pending");
  check(r.pc == kOrdinaryPc, "masked IRQ6 never diverted PC to the handler");
}

void mask_boundary_matches_level_6() {
  for (uint32_t m = 0; m <= 7; ++m) {
    GenesisRuntime r;
    prime(r);
    r.sr = (uint16_t)(0x2000u | (m << 8));
    bool admitted = false;
    g_step = [&](GenesisRuntime *rt) -> GenesisControlTransfer {
      if (rt->pc == kHandler) {
        admitted = true;
        GenesisControlTransfer t{};
        t.kind = GENESIS_COMPLETE;
        return t;
      }
      return continue_at(kOrdinaryPc);
    };
    (void)genesis_runtime_run(&r, trampoline, 64u);
    check(admitted == (m < 6u), "IRQ6 eligible iff SR mask < 6");
  }
}

// ---------------------------------------------------------------------------
// Item 6: exception-entry frame layout, SR/mask/mode transition, A7 movement,
// big-endian frame writes; user-mode (S=0) rejected; stack-wrap fails closed
// with no frame byte written.
void exception_entry_frame_is_correct() {
  GenesisRuntime r;
  prime(r);
  const uint32_t a7_before = r.a[7];
  const uint16_t sr_before = r.sr;
  bool checked = false;
  g_step = [&](GenesisRuntime *rt) -> GenesisControlTransfer {
    if (rt->pc == kHandler && !checked) {
      checked = true;
      const uint32_t base = a7_before - 6u;
      check(rt->a[7] == base, "A7 decremented by 6");
      // SR saved big-endian at base..base+1
      const uint16_t saved_sr = (uint16_t)((ram_byte(*rt, base) << 8) | ram_byte(*rt, base + 1u));
      check(saved_sr == sr_before, "saved SR is the pre-exception SR, big-endian");
      // PC saved big-endian at base+2..base+5
      const uint32_t saved_pc = ((uint32_t)ram_byte(*rt, base + 2u) << 24) |
                                ((uint32_t)ram_byte(*rt, base + 3u) << 16) |
                                ((uint32_t)ram_byte(*rt, base + 4u) << 8) |
                                (uint32_t)ram_byte(*rt, base + 5u);
      check(saved_pc == kOrdinaryPc, "saved PC is the interrupted target, big-endian");
      check(((rt->sr >> 8) & 0x7u) == 6u, "SR interrupt mask raised to 6");
      check((rt->sr & 0x2000u) != 0u, "SR supervisor bit set");
      check((rt->sr & 0x8000u) == 0u, "SR trace bit cleared");
      check(rt->pc == kHandler, "PC is the resolved handler entry");
      GenesisControlTransfer t{};
      t.kind = GENESIS_COMPLETE;
      return t;
    }
    return continue_at(kOrdinaryPc);
  };
  const auto result = genesis_runtime_run(&r, trampoline, 64u);
  check(result.kind == GENESIS_COMPLETE && checked, "handler was entered exactly once");
}

void user_mode_admission_is_rejected() {
  GenesisRuntime r;
  prime(r);
  r.sr = 0x0000u;  // S = 0 (user mode), mask 0
  g_step = [&](GenesisRuntime *rt) -> GenesisControlTransfer {
    (void)rt;
    return continue_at(kOrdinaryPc);
  };
  const auto result = genesis_runtime_run(&r, trampoline, 64u);
  check(result.kind == GENESIS_STOP &&
            result.stop.stop_class == GENESIS_STOP_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT &&
            result.stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT,
        "user-mode (S=0) admission fails closed");
  check(r.pc == kOrdinaryPc && r.a[7] == kRamBegin + 0x8000u, "no CPU mutation on user-mode reject");
}

// A cartridge whose IRQ6 vector slot points into work RAM (a RAM jump stub) has no statically rooted handler. The
// recognized VBlank interrupt then fails closed instead of being silently dropped, and mutates nothing.
void ram_resident_irq6_vector_fails_closed() {
  GenesisRuntime r;
  prime(r);
  r.irq6_handler_present = 0u;
  r.irq6_handler_entry = 0u;
  r.irq6_vector_in_work_ram = 1u;
  g_step = [&](GenesisRuntime *rt) -> GenesisControlTransfer {
    (void)rt;
    return continue_at(kOrdinaryPc);
  };
  const auto result = genesis_runtime_run(&r, trampoline, 64u);
  check(result.kind == GENESIS_STOP &&
            result.stop.stop_class == GENESIS_STOP_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT &&
            result.stop.diagnostic_category == GENESIS_DIAG_IRQ6_VECTOR_IN_WORK_RAM,
        "RAM-resident IRQ6 vector: a recognized VBlank interrupt fails closed");
  check(r.pc == kOrdinaryPc && r.a[7] == kRamBegin + 0x8000u, "no CPU mutation on the RAM-vector stop");

  // Without the RAM-vector flag the historical behaviour is unchanged: no handler means the interrupt is inert.
  GenesisRuntime inert;
  prime(inert);
  inert.irq6_handler_present = 0u;
  const auto spun = genesis_runtime_run(&inert, trampoline, 64u);
  check(spun.kind == GENESIS_RUNNER_RESOURCE_LIMIT, "no handler and no RAM vector: IRQ6 stays inert");
}

void stack_wrap_fails_closed_with_no_frame_write() {
  GenesisRuntime r;
  prime(r);
  r.a[7] = kRamBegin + 2u;  // frame_base = a7 - 6 underflows the RAM region
  // Poison the bytes just below to detect any partial write.
  for (uint32_t i = 0; i < 8; ++i) r.work_ram[i] = 0xAAu;
  g_step = [&](GenesisRuntime *rt) -> GenesisControlTransfer {
    (void)rt;
    return continue_at(kOrdinaryPc);
  };
  const auto result = genesis_runtime_run(&r, trampoline, 64u);
  check(result.kind == GENESIS_STOP &&
            result.stop.stop_class == GENESIS_STOP_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT,
        "stack underflow admission fails closed before any write");
  check(r.a[7] == kRamBegin + 2u && r.pc == kOrdinaryPc, "no A7/PC mutation on failed frame");
  bool untouched = true;
  for (uint32_t i = 0; i < 8; ++i) untouched = untouched && (r.work_ram[i] == 0xAAu);
  check(untouched, "no partial frame byte written on failed validation");
}

// ---------------------------------------------------------------------------
// Item 7: RTE reads SR@SP and PC@SP+2, commits atomically; a routed-read
// failure leaves sr/pc/a[7] entirely unmodified.
void rte_atomic_restore_and_failure() {
  GenesisRuntime r{};
  r.a[7] = kRamBegin + 0x100u;
  r.sr = 0x2600u;
  r.pc = kHandler;
  const uint32_t sp = r.a[7];
  // Lay a synthetic frame: SR=0x2004 at sp, PC=0x00012345 at sp+2 (big-endian).
  r.work_ram[sp - kRamBegin] = 0x20u;
  r.work_ram[sp - kRamBegin + 1u] = 0x04u;
  r.work_ram[sp - kRamBegin + 2u] = 0x00u;
  r.work_ram[sp - kRamBegin + 3u] = 0x01u;
  r.work_ram[sp - kRamBegin + 4u] = 0x23u;
  r.work_ram[sp - kRamBegin + 5u] = 0x45u;
  uint32_t restored = 0;
  GenesisRuntimeStop s{};
  check(genesis_exception_return(&r, &restored, &s) == 1, "RTE succeeds on a valid frame");
  check(r.sr == 0x2004u, "RTE restores SR from SP");
  check(r.pc == 0x00012345u && restored == 0x00012345u, "RTE restores PC from SP+2");
  check(r.a[7] == sp + 6u, "RTE pops the 6-byte frame");

  // Routed-read failure: SP points at an unmapped address.
  GenesisRuntime bad{};
  bad.a[7] = 0x00300000u;  // even, but unmapped for a routed read
  bad.sr = 0x1234u;
  bad.pc = 0x0000BEEFu;
  const uint32_t a7_before = bad.a[7];
  uint32_t out = 0;
  GenesisRuntimeStop bs{};
  check(genesis_exception_return(&bad, &out, &bs) == 0, "RTE fails closed on a bad frame read");
  check(bad.sr == 0x1234u && bad.pc == 0x0000BEEFu && bad.a[7] == a7_before,
        "failed RTE leaves sr/pc/a7 completely unmodified");
}

// A7 is a 32-bit architectural register; only the machine-side stack bus
// address and out-of-band IRQ frame key are 24-bit physical addresses.
void high_alias_stack_entry_and_returns() {
  constexpr uint32_t alias_sp = 0xFFFF8000u;
  constexpr uint32_t canonical_sp = 0x00FF8000u;
  constexpr uint32_t physical_frame = 0x00FF7FFAu;
  GenesisRuntime canonical;
  GenesisRuntime aliased;
  prime(canonical);
  prime(aliased);
  canonical.a[7] = canonical_sp;
  aliased.a[7] = alias_sp;
  canonical.devices.interrupt.vblank_pending = aliased.devices.interrupt.vblank_pending = 1u;
  const auto canonical_entry = genesis_runtime_retire_m68k_instruction(&canonical, 4u, kOrdinaryPc);
  const auto aliased_entry = genesis_runtime_retire_m68k_instruction(&aliased, 4u, kOrdinaryPc);
  check(canonical_entry.kind == GENESIS_CONTINUE_AT_PC &&
            aliased_entry.kind == canonical_entry.kind && aliased_entry.next_pc == canonical_entry.next_pc &&
            aliased_entry.next_pc == kHandler &&
            aliased_entry.stop.stop_class == canonical_entry.stop.stop_class &&
            aliased_entry.stop.diagnostic_category == canonical_entry.stop.diagnostic_category,
        "IRQ6 alias and canonical entry have identical transfer and stop");
  check(canonical.a[7] == canonical_sp - 6u && aliased.a[7] == alias_sp - 6u &&
            canonical.pc == aliased.pc && canonical.sr == aliased.sr &&
            canonical.scheduler.master_ticks == aliased.scheduler.master_ticks &&
            std::memcmp(canonical.work_ram, aliased.work_ram, sizeof(canonical.work_ram)) == 0,
        "IRQ6 alias and canonical entry share physical frame, SR, PC and timing, but not high A7");
  check(ram_byte(aliased, physical_frame) == 0x20u && ram_byte(aliased, physical_frame + 1u) == 0x00u &&
            ram_byte(aliased, physical_frame + 4u) == 0x02u,
        "IRQ6 frame is stored at the physical work-RAM address");
  check(std::memcmp(canonical.exception_frame_irq6_origin, aliased.exception_frame_irq6_origin,
                    sizeof(canonical.exception_frame_irq6_origin)) == 0 &&
            canonical.exception_frame_irq6_origin[(physical_frame - kRamBegin) / 16u] != 0u,
        "both IRQ6 entries record the same physical frame origin");
  uint32_t canonical_restored = 0, aliased_restored = 0;
  GenesisRuntimeStop canonical_stop{}, aliased_stop{};
  const int canonical_ok = genesis_exception_return(&canonical, &canonical_restored, &canonical_stop);
  const int aliased_ok = genesis_exception_return(&aliased, &aliased_restored, &aliased_stop);
  check(canonical_ok == 1 && aliased_ok == canonical_ok && aliased_restored == canonical_restored &&
            aliased_restored == kOrdinaryPc && canonical_stop.stop_class == aliased_stop.stop_class &&
            canonical_stop.diagnostic_category == aliased_stop.diagnostic_category &&
            canonical.a[7] == canonical_sp && aliased.a[7] == alias_sp &&
            canonical.pc == aliased.pc && canonical.sr == aliased.sr &&
            canonical.scheduler.master_ticks == aliased.scheduler.master_ticks &&
            std::memcmp(canonical.work_ram, aliased.work_ram, sizeof(canonical.work_ram)) == 0,
        "canonical and aliased IRQ6 RTE return identically except for full-width A7");
  bool origins_consumed = true;
  for (uint8_t origin : aliased.exception_frame_irq6_origin) origins_consumed &= origin == 0u;
  check(origins_consumed && std::memcmp(canonical.exception_frame_irq6_origin, aliased.exception_frame_irq6_origin,
                                         sizeof(canonical.exception_frame_irq6_origin)) == 0,
        "both IRQ6 frame origins are consumed identically on RTE");

  canonical.a[7] = canonical_sp - 6u;
  aliased.a[7] = alias_sp - 6u;
  canonical.pc = aliased.pc = kHandler;
  canonical.sr = aliased.sr = 0x2700u;
  canonical.work_ram[physical_frame - kRamBegin] = aliased.work_ram[physical_frame - kRamBegin] = 0xFFu;
  canonical.work_ram[physical_frame - kRamBegin + 1u] =
      aliased.work_ram[physical_frame - kRamBegin + 1u] = 0x15u;
  canonical_restored = aliased_restored = 0;
  const int canonical_rtr = genesis_return_restore_condition_codes(&canonical, &canonical_restored, &canonical_stop);
  const int aliased_rtr = genesis_return_restore_condition_codes(&aliased, &aliased_restored, &aliased_stop);
  check(canonical_rtr == 1 && aliased_rtr == canonical_rtr && aliased_restored == canonical_restored &&
            aliased_restored == kOrdinaryPc && canonical_stop.stop_class == aliased_stop.stop_class &&
            canonical_stop.diagnostic_category == aliased_stop.diagnostic_category &&
            canonical.a[7] == canonical_sp && aliased.a[7] == alias_sp &&
            canonical.pc == aliased.pc && canonical.sr == aliased.sr && aliased.sr == 0x2715u &&
            canonical.scheduler.master_ticks == aliased.scheduler.master_ticks &&
            std::memcmp(canonical.work_ram, aliased.work_ram, sizeof(canonical.work_ram)) == 0,
        "canonical and aliased RTR restore identically except for full-width A7");

  // Valid first word but a long read crossing the physical RAM end: reject
  // before either CPU register or the output target is changed.
  GenesisRuntime &r = aliased;
  uint32_t &restored = aliased_restored;
  GenesisRuntimeStop &stop = aliased_stop;
  r.a[7] = 0xFFFFFFFCu;
  r.sr = 0x2700u;
  r.pc = kHandler;
  restored = 0xBEEFu;
  const GenesisRuntime before_bad_frame = r;
  check(genesis_exception_return(&r, &restored, &stop) == 0 &&
            stop.diagnostic_category == GENESIS_DIAG_INVALID_STACK_RANGE &&
            std::memcmp(&r, &before_bad_frame, sizeof(r)) == 0 && restored == 0xBEEFu,
        "aliased boundary-crossing RTE fails before any read or accounting");
  check(genesis_return_restore_condition_codes(&r, &restored, &stop) == 0 &&
            stop.diagnostic_category == GENESIS_DIAG_INVALID_STACK_RANGE &&
            std::memcmp(&r, &before_bad_frame, sizeof(r)) == 0 && restored == 0xBEEFu,
        "aliased boundary-crossing RTR fails before any read or accounting");

  prime(r);
  r.a[7] = 0xFFFF0002u;
  r.work_ram[0] = 0xAAu;
  r.devices.interrupt.vblank_pending = 1u;
  const auto rejected = genesis_runtime_retire_m68k_instruction(&r, 4u, kOrdinaryPc);
  check(rejected.kind == GENESIS_STOP && r.a[7] == 0xFFFF0002u &&
            r.pc == kOrdinaryPc && r.work_ram[0] == 0xAAu,
        "aliased IRQ frame crossing physical RAM start fails before writing");

  prime(r);
  r.a[7] = alias_sp | 1u;
  for (uint32_t offset = physical_frame - kRamBegin; offset < physical_frame - kRamBegin + 8u; ++offset)
    r.work_ram[offset] = 0xAAu;
  r.devices.interrupt.vblank_pending = 1u;
  const auto odd = genesis_runtime_retire_m68k_instruction(&r, 4u, kOrdinaryPc);
  bool frame_untouched = true;
  for (uint32_t offset = physical_frame - kRamBegin; offset < physical_frame - kRamBegin + 8u; ++offset)
    frame_untouched &= r.work_ram[offset] == 0xAAu;
  check(odd.kind == GENESIS_STOP && odd.stop.stop_class == GENESIS_STOP_UNSUPPORTED_INTERRUPT_OR_SCHEDULING_EVENT &&
            r.a[7] == (alias_sp | 1u) && r.pc == kOrdinaryPc && frame_untouched,
        "odd aliased stack rejects IRQ6 entry without writing a frame");
  restored = 0xBEEFu;
  check(genesis_exception_return(&r, &restored, &stop) == 0 && r.a[7] == (alias_sp | 1u) &&
            restored == 0xBEEFu && r.pc == kOrdinaryPc,
        "odd aliased RTE stack fails without committing");
  check(genesis_return_restore_condition_codes(&r, &restored, &stop) == 0 &&
            r.a[7] == (alias_sp | 1u) && restored == 0xBEEFu && r.pc == kOrdinaryPc,
        "odd aliased RTR stack fails without committing");
}

void high_alias_synchronous_entry_and_rte() {
  constexpr uint32_t alias_sp = 0xFFFF4000u;
  constexpr uint32_t canonical_sp = 0x00FF4000u;
  GenesisRuntime canonical{}, aliased{};
  for (GenesisRuntime *r : {&canonical, &aliased}) {
    r->a[7] = r == &canonical ? canonical_sp : alias_sp;
    r->sr = 0x2015u;
    r->pc = kOrdinaryPc;
    r->software_exception_handler_present[4] = 1u;
    r->software_exception_handler_entry[4] = kHandler;
  }
  uint32_t canonical_handler = 0, aliased_handler = 0;
  GenesisRuntimeStop canonical_stop{}, aliased_stop{};
  const int canonical_ok = genesis_raise_software_exception(&canonical, 4u, kOrdinaryPc, 34u,
                                                            &canonical_handler, &canonical_stop);
  const int aliased_ok = genesis_raise_software_exception(&aliased, 4u, kOrdinaryPc, 34u,
                                                          &aliased_handler, &aliased_stop);
  check(canonical_ok == 1 && aliased_ok == canonical_ok && aliased_handler == canonical_handler &&
            aliased_handler == kHandler && canonical.a[7] == canonical_sp - 6u &&
            aliased.a[7] == alias_sp - 6u && canonical.pc == aliased.pc && canonical.sr == aliased.sr &&
            canonical_stop.stop_class == aliased_stop.stop_class &&
            canonical_stop.diagnostic_category == aliased_stop.diagnostic_category &&
            canonical.scheduler.master_ticks == aliased.scheduler.master_ticks &&
            std::memcmp(canonical.work_ram, aliased.work_ram, sizeof(canonical.work_ram)) == 0,
        "synchronous exception alias matches canonical frame, CPU state, stop and timing except A7");
  check(ram_byte(aliased, 0x00FF3FFAu) == 0x20u && ram_byte(aliased, 0x00FF3FFBu) == 0x15u,
        "synchronous frame lands at physical RAM alias");
  uint32_t canonical_restored = 0, aliased_restored = 0;
  const int canonical_rte = genesis_exception_return(&canonical, &canonical_restored, &canonical_stop);
  const int aliased_rte = genesis_exception_return(&aliased, &aliased_restored, &aliased_stop);
  check(canonical_rte == 1 && aliased_rte == canonical_rte && aliased_restored == canonical_restored &&
            aliased_restored == kOrdinaryPc && canonical.a[7] == canonical_sp && aliased.a[7] == alias_sp &&
            canonical.pc == aliased.pc && canonical.sr == aliased.sr && aliased.sr == 0x2015u &&
            canonical_stop.stop_class == aliased_stop.stop_class &&
            canonical_stop.diagnostic_category == aliased_stop.diagnostic_category &&
            canonical.scheduler.master_ticks == aliased.scheduler.master_ticks &&
            std::memcmp(canonical.work_ram, aliased.work_ram, sizeof(canonical.work_ram)) == 0,
        "synchronous canonical and aliased RTE restore identically except for full-width A7");
}

// ---------------------------------------------------------------------------
// Item 8-style integration: ordinary code -> VBlank -> pending IRQ6 -> admission
// -> handler mutates a RAM flag -> RTE -> resumed code observes the flag ->
// exits its spin. Run twice, identical.
GenesisControlTransfer integration_run(uint32_t *out_flag_addr_value) {
  GenesisRuntime r;
  prime(r);
  const uint32_t flag_addr = kRamBegin + 0x40u;
  g_step = [&](GenesisRuntime *rt) -> GenesisControlTransfer {
    if (rt->pc == kHandler) {
      // Handler sets the RAM flag through the routed boundary, then RTEs.
      uint32_t one = 1u;
      GenesisRuntimeStop ws{};
      genesis_route_access(rt, flag_addr, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, &one, &ws);
      uint32_t restored = 0;
      GenesisRuntimeStop s{};
      genesis_exception_return(rt, &restored, &s);
      return continue_at(restored);
    }
    // Ordinary code: spin until the flag is non-zero, then complete.
    uint32_t v = 0;
    GenesisRuntimeStop rs{};
    genesis_route_access(rt, flag_addr, GENESIS_ACCESS_WORD, GENESIS_ACCESS_READ, &v, &rs);
    if (v != 0u) {
      GenesisControlTransfer t{};
      t.kind = GENESIS_COMPLETE;
      return t;
    }
    return continue_at(kOrdinaryPc);
  };
  const auto result = genesis_runtime_run(&r, trampoline, 4096u);
  *out_flag_addr_value = ram_byte(r, flag_addr + 1u);
  return result;
}

void integration_round_trip_is_deterministic() {
  uint32_t a = 0, b = 0;
  const auto ra = integration_run(&a);
  const auto rb = integration_run(&b);
  check(ra.kind == GENESIS_COMPLETE && rb.kind == GENESIS_COMPLETE,
        "integration round trip completes via a real admitted IRQ6 + RTE");
  check(a == 1u && b == 1u, "handler's RAM flag mutation is observed by resumed code");
  check(ra.kind == rb.kind, "repeated integration run is deterministic");
}

// A returned IRQ frame restores an eligible SR.  With no synthetic post-RTE
// grace, an already-pending IRQ6 is admitted by the first completed resumed
// instruction, not deferred to a later dispatch boundary.
void post_rte_first_retirement_admits_pending_irq6() {
  GenesisRuntime r;
  prime(r);
  const uint32_t sp = r.a[7];
  r.pc = kHandler;
  r.work_ram[sp - kRamBegin + 0u] = 0x20u;
  r.work_ram[sp - kRamBegin + 1u] = 0x00u;
  r.work_ram[sp - kRamBegin + 2u] = 0x00u;
  r.work_ram[sp - kRamBegin + 3u] = 0x00u;
  r.work_ram[sp - kRamBegin + 4u] = (uint8_t)(kOrdinaryPc >> 8);
  r.work_ram[sp - kRamBegin + 5u] = (uint8_t)kOrdinaryPc;
  r.devices.interrupt.vblank_pending = 1u;

  uint32_t restored = 0;
  GenesisRuntimeStop stop{};
  check(genesis_exception_return(&r, &restored, &stop) == 1 && restored == kOrdinaryPc,
        "RTE restores an eligible mainline target");
  const GenesisControlTransfer result = genesis_runtime_retire_m68k_instruction(&r, 4u, kOrdinaryPc);
  check(result.kind == GENESIS_CONTINUE_AT_PC && result.next_pc == kHandler && r.pc == kHandler,
        "first resumed retirement immediately admits an eligible pending IRQ6");
  check(r.devices.interrupt.vblank_pending == 0u,
        "immediate post-RTE admission acknowledges the pending IRQ6");
}

// ---------------------------------------------------------------------------
// Superseded by ADR-0041: these grace-policy regressions document the removed
// dispatch-step scheduler behavior and must not participate in C++14 builds.
#if 0
// SEG-007-T047 / ADR-0020 §5+§9 adversarial: a successful RTE arms a true
// one-boundary admission grace so the resumed instruction stream always makes
// progress before the next IRQ6 admission -- even when the handler ran LONGER
// than one VBlank scheduler period (so the scheduler re-armed vblank_pending
// mid-handler) and its RTE lowers the mask below 6. The grace is an explicit
// bounded project compatibility policy forced by the coarse dispatch-step
// synthetic VBlank scheduler, not an architected MC68000 rule.
[[maybe_unused]] void post_rte_grace_guarantees_resumed_progress() {
  GenesisRuntime r;
  prime(r);
  const uint32_t counter_addr = kRamBegin + 0x40u;  // mainline-only side effect
  const int kLongHandler = 3;  // > one virtual frame
  int handler_steps = 0;
  int in_handler = 0;
  int handler_entries = 0;
  int mainline_steps_since_rte = 0;
  int min_gap_after_rte = -1;
  int guard = 0;

  g_step = [&](GenesisRuntime *rt) -> GenesisControlTransfer {
    if (++guard > 8000) {
      GenesisControlTransfer t{};
      t.kind = GENESIS_COMPLETE;
      return t;
    }
    if (rt->pc == kHandler) {
      if (!in_handler) {
        ++handler_entries;  // fresh admission / handler re-entry
        in_handler = 1;
        handler_steps = 0;
        if (handler_entries >= 2 &&
            (min_gap_after_rte < 0 || mainline_steps_since_rte < min_gap_after_rte))
          min_gap_after_rte = mainline_steps_since_rte;
        if (handler_entries >= 4) {
          GenesisControlTransfer t{};
          t.kind = GENESIS_COMPLETE;
          return t;
        }
      }
      if (++handler_steps < kLongHandler) return continue_at(kHandler);
      uint32_t restored = 0;
      GenesisRuntimeStop s{};
      check(genesis_exception_return(rt, &restored, &s) == 1, "long handler RTE succeeds");
      in_handler = 0;
      mainline_steps_since_rte = 0;
      return continue_at(restored);
    }
    // Mainline: a routed RAM write side effect + a mainline-only retire counter.
    ++mainline_steps_since_rte;
    uint32_t v = (uint32_t)mainline_steps_since_rte;
    GenesisRuntimeStop ws{};
    genesis_route_access(rt, counter_addr, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, &v, &ws);
    return continue_at(kOrdinaryPc);
  };
  const auto result = genesis_runtime_run(&r, trampoline, 4096u);
  check(result.kind == GENESIS_COMPLETE, "long-handler grace scenario completes");
  check(handler_entries >= 2, "IRQ6 is re-admitted after the long handler returns");
  check(min_gap_after_rte >= 1,
        "resumed mainline runs >=1 dispatch step after RTE before the next IRQ6 admission");
  check(ram_byte(r, counter_addr + 1u) >= 1u,
        "a mainline RAM write lands after RTE before handler re-entry");
}

// A normal short-handler round trip is still re-admitted on a correct later
// VBlank period -- never on the immediately-post-RTE step, and never suppressed
// indefinitely.
[[maybe_unused]] void short_handler_readmits_on_later_period_only() {
  GenesisRuntime r;
  prime(r);
  int handler_entries = 0;
  int in_handler = 0;
  int mainline_since_rte = 0;
  int min_gap = -1;
  int max_gap = -1;
  int guard = 0;
  g_step = [&](GenesisRuntime *rt) -> GenesisControlTransfer {
    if (++guard > 6000) {
      GenesisControlTransfer t{};
      t.kind = GENESIS_COMPLETE;
      return t;
    }
    if (rt->pc == kHandler) {
      if (!in_handler) {
        ++handler_entries;
        in_handler = 1;
        if (handler_entries >= 2) {
          if (min_gap < 0 || mainline_since_rte < min_gap) min_gap = mainline_since_rte;
          if (mainline_since_rte > max_gap) max_gap = mainline_since_rte;
        }
        if (handler_entries >= 6) {
          GenesisControlTransfer t{};
          t.kind = GENESIS_COMPLETE;
          return t;
        }
      }
      uint32_t restored = 0;
      GenesisRuntimeStop s{};
      check(genesis_exception_return(rt, &restored, &s) == 1, "short handler RTE succeeds");
      in_handler = 0;
      mainline_since_rte = 0;
      return continue_at(restored);
    }
    ++mainline_since_rte;
    return continue_at(kOrdinaryPc);
  };
  const auto result = genesis_runtime_run(&r, trampoline, 4096u);
  check(result.kind == GENESIS_COMPLETE, "short-handler scenario completes");
  check(handler_entries >= 6, "short handler keeps being re-admitted on later periods");
  check(min_gap >= 1, "short handler never re-admits on the immediately-post-RTE step");
  check(max_gap <= 3,
        "short handler re-admits on the correct later period, grace is not an indefinite block");
}

// ---------------------------------------------------------------------------
// SEG-007-T047 / ADR-0020 §5 step 1a regression: the post-RTE grace is a TRUE
// one-boundary grace. A successful RTE that occurs while vblank_pending == 0
// must not leave a grace token alive across the following resumed-mainline
// dispatches so that it later declines a genuinely new VBlank rising edge. The
// new edge must be admitted on the very first eligible step after it becomes
// pending.
[[maybe_unused]] void stale_grace_does_not_suppress_a_new_vblank() {
  GenesisRuntime r;
  prime(r);
  const uint32_t sp = r.a[7];
  r.pc = kHandler;
  // Synthetic 6-byte frame: SR = 0x2000 (S=1, mask 0) at sp, PC = kOrdinaryPc
  // at sp+2, big-endian -- exactly the shape genesis_exception_return consumes.
  r.work_ram[sp - kRamBegin + 0u] = 0x20u;
  r.work_ram[sp - kRamBegin + 1u] = 0x00u;
  r.work_ram[sp - kRamBegin + 2u] = 0x00u;
  r.work_ram[sp - kRamBegin + 3u] = 0x00u;
  r.work_ram[sp - kRamBegin + 4u] = (uint8_t)(kOrdinaryPc >> 8);
  r.work_ram[sp - kRamBegin + 5u] = (uint8_t)kOrdinaryPc;
  bool did_rte = false;
  const int kEdgeStep = 3;  // several resumed-mainline dispatches run first
  int mainline_after_rte = 0;
  int handler_entries = 0;
  int mainline_at_first_admit = -1;
  int guard = 0;
  g_step = [&](GenesisRuntime *rt) -> GenesisControlTransfer {
    if (++guard > 4000) {
      GenesisControlTransfer t{};
      t.kind = GENESIS_COMPLETE;
      return t;
    }
    if (!did_rte) {
      check(rt->devices.interrupt.vblank_pending == 0u, "no VBlank pending at the initial RTE");
      uint32_t restored = 0;
      GenesisRuntimeStop s{};
      check(genesis_exception_return(rt, &restored, &s) == 1, "initial RTE succeeds");
      did_rte = true;
      return continue_at(restored);
    }
    if (rt->pc == kHandler) {
      ++handler_entries;
      if (mainline_at_first_admit < 0) mainline_at_first_admit = mainline_after_rte;
      uint32_t restored = 0;
      GenesisRuntimeStop s{};
      check(genesis_exception_return(rt, &restored, &s) == 1, "later handler RTE succeeds");
      GenesisControlTransfer t{};
      t.kind = GENESIS_COMPLETE;
      return t;
    }
    ++mainline_after_rte;
    // Only after several resumed-mainline steps does a genuinely new VBlank
    // rising edge become pending (driven here exactly as the device scheduler
    // would raise it, well after the initial RTE).
    if (mainline_after_rte == kEdgeStep) {
      rt->devices.interrupt.vblank_pending = 1u;
      ++rt->devices.interrupt.vblank_transition_count;
    }
    return continue_at(kOrdinaryPc);
  };
  const auto result = genesis_runtime_run(&r, trampoline, 4096u);
  check(result.kind == GENESIS_COMPLETE, "stale-grace scenario completes");
  check(handler_entries == 1,
        "the genuinely new VBlank rising edge after the initial RTE is admitted "
        "(a stale post-RTE grace did not suppress it)");
  check(mainline_at_first_admit == kEdgeStep,
        "the new IRQ6 is admitted on the very first eligible step after its rising edge -- "
        "the one-boundary grace was already spent on the first post-RTE step");
}
#endif

}  // namespace

int main() {
  scheduler_recurs_and_gates();
  masked_irq_stays_pending_under_runner_resource_limit();
  mask_boundary_matches_level_6();
  exception_entry_frame_is_correct();
  user_mode_admission_is_rejected();
  ram_resident_irq6_vector_fails_closed();
  stack_wrap_fails_closed_with_no_frame_write();
  rte_atomic_restore_and_failure();
  high_alias_stack_entry_and_returns();
  high_alias_synchronous_entry_and_rte();
  integration_round_trip_is_deterministic();
  post_rte_first_retirement_admits_pending_irq6();
  if (failures == 0) std::printf("ok\n");
  return failures == 0 ? 0 : 1;
}
