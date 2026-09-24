// SEG-007-T222 / ADR-0037: focused unit coverage of the synchronous
// divide-by-zero (vector 5) CPU exception seam -- genesis_raise_divide_by_zero
// and the shared exception-frame-construction helper it reuses from ADR-0020.
// Synthetic-only: no target opcode, image, or decoder is involved (this test
// drives runtime.c's public entry points directly, exactly like the existing
// IRQ6 interrupt test does for its own mechanism).

#include "runtime.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>

namespace {

int failures = 0;

void check(bool ok, const char *what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

constexpr uint32_t kRamBegin = 0x00FF0000u;
constexpr uint32_t kHandler = 0x00002000u;
constexpr uint32_t kFaultPc = 0x00000B02u;  // instruction following the faulting DIVS.W/DIVU.W

void prime(GenesisRuntime &r) {
  r = GenesisRuntime{};
  r.a[7] = kRamBegin + 0x8000u;
  r.pc = 0x00000B00u;
  r.sr = 0x2000u;  // S = 1, mask = 0, T = 0
  r.divide_by_zero_handler_entry = kHandler;
  r.divide_by_zero_handler_present = 1u;
}

uint8_t ram_byte(const GenesisRuntime &r, uint32_t addr) { return r.work_ram[addr - kRamBegin]; }

// The synchronous path saves the original SR, clears trace and forces S, but
// deliberately preserves the current interrupt mask (unlike IRQ6).
void frame_is_correct_and_rte_restores_for_each_interrupt_mask() {
  for (uint16_t mask : {uint16_t{0u}, uint16_t{3u}, uint16_t{7u}}) {
  GenesisRuntime r;
  prime(r);
    const uint16_t mask_bits = static_cast<uint16_t>(static_cast<unsigned>(mask) << 8U);
    r.sr = static_cast<uint16_t>(0xA000u | 0x2000u | mask_bits);
  const uint16_t saved_sr = r.sr;
  uint32_t handler_pc = 0;
  GenesisRuntimeStop stop{};
  const int ok = genesis_raise_divide_by_zero(&r, kFaultPc, &handler_pc, &stop);
  check(ok == 1, "divide-by-zero raise succeeds with a build-resolved handler");
  check(handler_pc == kHandler, "raise jumps to the build-resolved vector-5 handler entry");
  check(r.pc == kHandler, "runtime->pc is set to the handler entry");
  check(r.a[7] == kRamBegin + 0x8000u - 6u, "A7 is decremented by exactly six bytes");
  const uint32_t frame_base = kRamBegin + 0x8000u - 6u;
  const uint16_t frame_sr =
      static_cast<uint16_t>((ram_byte(r, frame_base) << 8) | ram_byte(r, frame_base + 1u));
  check(frame_sr == saved_sr, "pushed SR word matches the pre-exception SR");
  const uint32_t frame_pc = (static_cast<uint32_t>(ram_byte(r, frame_base + 2u)) << 24) |
                            (static_cast<uint32_t>(ram_byte(r, frame_base + 3u)) << 16) |
                            (static_cast<uint32_t>(ram_byte(r, frame_base + 4u)) << 8) |
                            static_cast<uint32_t>(ram_byte(r, frame_base + 5u));
  check(frame_pc == kFaultPc, "pushed PC long word is the instruction following the faulting DIVS/DIVU");
  check((r.sr & 0x2000u) != 0u, "S bit forced to 1 (already was)");
    check((r.sr & 0x0700u) == mask_bits, "synchronous entry preserves interrupt mask");
  check((r.sr & 0x8000u) == 0u, "T bit cleared");

  // SEG-021-T018 / ADR 0043 §6: the stacked SR has T = 1, so RTE would leave T set -- trace is deferred and
  // the return fails closed with nothing restored.
  {
    const auto before_rte = r;
    uint32_t restored_pc = 0;
    GenesisRuntimeStop rte_stop{};
    check(genesis_exception_return(&r, &restored_pc, &rte_stop) == 0, "RTE of a T = 1 frame fails closed");
    check(rte_stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_EXCEPTION &&
              rte_stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_TRACE_EXCEPTION,
          "deferred trace is the explicit unsupported-CPU-exception stop");
    check(r.sr == before_rte.sr && r.pc == before_rte.pc && r.a[7] == before_rte.a[7] && r.usp == before_rte.usp,
          "a deferred-trace RTE restores nothing");
  }
  // With T = 0 in the stacked SR, RTE restores the exact pre-exception state.
  r.work_ram[frame_base - kRamBegin] = static_cast<uint8_t>((saved_sr & 0x7FFFu) >> 8U);
  uint32_t restored_pc = 0;
  GenesisRuntimeStop rte_stop{};
  check(genesis_exception_return(&r, &restored_pc, &rte_stop) == 1, "RTE restoration succeeds");
  check(restored_pc == kFaultPc, "RTE restores the exact pushed fault PC");
  check(r.sr == (saved_sr & 0x7FFFu), "RTE restores the exact pre-exception SR (T clear)");
  check(r.a[7] == kRamBegin + 0x8000u, "RTE restores A7 to its pre-exception value");
  }
}

// No build-resolved handler installed: fails closed, no state mutated.
void no_handler_fails_closed_with_no_mutation() {
  GenesisRuntime r;
  prime(r);
  r.divide_by_zero_handler_present = 0u;
  const auto saved = r;
  uint32_t handler_pc = 0;
  GenesisRuntimeStop stop{};
  const int ok = genesis_raise_divide_by_zero(&r, kFaultPc, &handler_pc, &stop);
  check(ok == 0, "no handler installed fails closed");
  check(stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM, "stop class is CPU-form (synchronous exception delivery)");
  check(stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DIVIDE_BY_ZERO_EXCEPTION,
        "diagnostic category names the divide-by-zero delivery failure");
  check(r.pc == saved.pc && r.sr == saved.sr && r.a[7] == saved.a[7], "no CPU state mutated on failure");
}

// SEG-021-T018 / ADR 0043 §5/§6 (supersedes the former supervisor-only entry): user mode (S == 0) at the fault
// enters on the SSP (the inactive slot), saves SR with S = 0 and moves the USP to the inactive slot; RTE of that
// frame restores S = 0, re-activates the USP and parks the incremented SSP in the inactive slot.
void user_mode_entry_switches_to_the_supervisor_stack() {
  GenesisRuntime r;
  prime(r);
  r.sr = 0x0015u;  // S = 0, mask 0, X/Z/C set
  const uint32_t user_sp = kRamBegin + 0x8000u;
  const uint32_t supervisor_sp = kRamBegin + 0x9000u;
  r.a[7] = user_sp;
  r.usp = supervisor_sp;
  const auto saved_user_ram = r.work_ram[0x8000u - 6u];
  uint32_t handler_pc = 0;
  GenesisRuntimeStop stop{};
  check(genesis_raise_divide_by_zero(&r, kFaultPc, &handler_pc, &stop) == 1, "user-mode vector 5 enters");
  check(r.a[7] == supervisor_sp - 6u, "the frame is on the SSP");
  check(r.usp == user_sp, "the USP moves to the inactive slot");
  check(r.sr == 0x2015u, "entry sets S and keeps the mask and CCR");
  check(ram_byte(r, supervisor_sp - 6u) == 0x00u && ram_byte(r, supervisor_sp - 5u) == 0x15u,
        "the stacked SR has S = 0");
  check(r.work_ram[0x8000u - 6u] == saved_user_ram, "nothing is written on the user stack");
  uint32_t restored_pc = 0;
  GenesisRuntimeStop rte_stop{};
  check(genesis_exception_return(&r, &restored_pc, &rte_stop) == 1, "RTE to user mode succeeds");
  check(r.sr == 0x0015u && r.pc == kFaultPc, "RTE restores the user SR and PC");
  check(r.a[7] == user_sp && r.usp == supervisor_sp, "RTE re-activates the USP and parks the SSP");
}

// NOT gated by the SR interrupt mask (unlike IRQ6). A fully-masked SR must
// still admit the synchronous exception, and the mask survives entry.
void not_gated_by_interrupt_mask() {
  GenesisRuntime r;
  prime(r);
  r.sr = 0x2700u;  // S = 1, mask = 7 (would block IRQ6 entirely)
  uint32_t handler_pc = 0;
  GenesisRuntimeStop stop{};
  const int ok = genesis_raise_divide_by_zero(&r, kFaultPc, &handler_pc, &stop);
  check(ok == 1, "divide-by-zero is admitted even with the SR interrupt mask fully closed");
  check(handler_pc == kHandler, "handler still resolved correctly under a masked SR");
  check((r.sr & 0x0700u) == 0x0700u, "fully closed interrupt mask remains closed in the handler");
}

// Does not touch IRQ6 virtual-time or pending state.
void does_not_touch_irq6_scheduler_state() {
  GenesisRuntime r;
  prime(r);
  r.scheduler.master_ticks = 42u;
  r.devices.interrupt.vblank_pending = 1u;
  uint32_t handler_pc = 0;
  GenesisRuntimeStop stop{};
  check(genesis_raise_divide_by_zero(&r, kFaultPc, &handler_pc, &stop) == 1, "raise succeeds");
  check(r.scheduler.master_ticks == 42u, "IRQ6 virtual-time phase untouched");
  check(r.devices.interrupt.vblank_pending == 1u, "VBlank-pending flag untouched (not re-armed/consumed)");
  uint32_t restored_pc = 0;
  GenesisRuntimeStop rte_stop{};
  check(genesis_exception_return(&r, &restored_pc, &rte_stop) == 1, "vector-5 RTE succeeds");
  check(restored_pc == kFaultPc, "vector-5 RTE restores the saved PC");
  check(r.scheduler.master_ticks == 42u,
        "vector-5 RTE leaves IRQ6 virtual time untouched");
}

}  // namespace

int main() {
  frame_is_correct_and_rte_restores_for_each_interrupt_mask();
  no_handler_fails_closed_with_no_mutation();
  user_mode_entry_switches_to_the_supervisor_stack();
  not_gated_by_interrupt_mask();
  does_not_touch_irq6_scheduler_state();
  if (failures == 0) std::printf("ok\n");
  return failures == 0 ? 0 : 1;
}
