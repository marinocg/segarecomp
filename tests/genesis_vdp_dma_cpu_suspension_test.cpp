// SEG-007-T175: focused unit coverage of the 68000 bus-stall/suspension
// modeled for a GENESIS_VDP_DMA_MEMORY_TO_VRAM transfer. Real Genesis
// hardware stops the 68000 from fetching or executing any further
// instruction while a memory-to-VDP DMA transfer owns the bus (see the VDP
// DMA citations already referenced by SEG-007-T084/T091/T098/T101/T108 and
// docs/references/genesis-vdp-data-port-cpu-write-contract.md). The PRIMARY
// suspension mechanism is a synchronous drain performed as a device-side
// effect of the exact routed CONTROL-port write that arms the DMA
// (genesis_route_access's own VDP branch), so the invariant holds at
// instruction granularity, not merely at a generated-dispatch-call boundary;
// genesis_runtime_run's own pre-dispatch drain is only a defensive
// secondary mechanism. `arming_write_drains_synchronously_within_same_
// dispatch_call` below exercises the real routed-access seam
// (genesis_route_access) within a single synthetic dispatch call to
// discriminate the two granularities; the remaining cases drive
// genesis_runtime_run directly with a synthetic dispatch function that
// observes DMA state, mirroring the pattern already established by
// tests/genesis_runtime_run_test.cpp. No target opcode, image, or
// decoder is involved.

#include "runtime.h"

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

// A synthetic dispatch closure. genesis_runtime_run takes a plain function
// pointer, so route through a file-scope std::function.
std::function<GenesisControlTransfer(GenesisRuntime *)> g_step;

GenesisControlTransfer trampoline(GenesisRuntime *runtime) { return g_step(runtime); }

GenesisControlTransfer continue_at(uint32_t pc) {
  GenesisControlTransfer t{};
  t.kind = GENESIS_CONTINUE_AT_PC;
  t.next_pc = pc;
  return t;
}

GenesisControlTransfer own_frontier_stop() {
  GenesisControlTransfer t{};
  t.kind = GENESIS_STOP;
  t.stop.stop_class = GENESIS_STOP_UNSUPPORTED_CPU_FORM;
  return t;
}

// Case: a generated CPU block arms a multi-word memory-to-VRAM DMA. The
// following generated CPU dispatch step must not observe the DMA as still
// BUSY (i.e. it must not run until the transfer engine reaches
// GENESIS_VDP_DMA_IDLE), every programmed word must transfer in order with
// unchanged source/target/address/count progression, and the resumed
// dispatch step's own subsequent frontier is reached normally.
void multi_word_dma_drains_before_next_dispatch_step() {
  GenesisRuntime runtime{};
  // Two source words in work RAM ($00FF0000), the DMA source family
  // genesis_vdp_dma_source_is_routable already accepts unconditionally.
  runtime.work_ram[0] = 0xABU; runtime.work_ram[1] = 0xCDU;
  runtime.work_ram[2] = 0x12U; runtime.work_ram[3] = 0x34U;

  int step = 0;
  int observed_busy_on_later_step = 0;
  g_step = [&](GenesisRuntime *r) -> GenesisControlTransfer {
    ++step;
    if (step == 1) {
      r->devices.vdp.dma.phase = GENESIS_VDP_DMA_BUSY;
      r->devices.vdp.dma.kind = GENESIS_VDP_DMA_MEMORY_TO_VRAM;
      r->devices.vdp.dma.source_address = 0x00FF0000U;
      r->devices.vdp.dma.remaining_length = 2U;
      r->devices.vdp.dma.write_target_code = 0x01U; /* VRAM */
      r->devices.vdp.addressed_pointer = 0U;
      r->devices.vdp.auto_increment_value = 2U;
      return continue_at(0x10U);
    }
    /* This is the "following generated CPU instruction": it must only ever
       run once genesis_runtime_run has drained the DMA to IDLE first. */
    if (r->devices.vdp.dma.phase != GENESIS_VDP_DMA_IDLE) ++observed_busy_on_later_step;
    return own_frontier_stop();
  };
  const auto result = genesis_runtime_run(&runtime, trampoline, 128U);
  check(step == 2, "exactly the arming block and the resumed block are dispatched");
  check(observed_busy_on_later_step == 0,
        "the following generated CPU instruction never observes the DMA still BUSY");
  check(runtime.devices.vdp.dma.phase == GENESIS_VDP_DMA_IDLE,
        "the DMA reaches IDLE before generated dispatch resumes");
  check(runtime.devices.vdp.dma.remaining_length == 0U, "every programmed word transferred");
  check(runtime.devices.vdp.dma.transfer_access_count == 2U,
        "exactly the programmed word count was transferred (unchanged progression)");
  check(runtime.devices.vdp.vram[0] == 0xABU && runtime.devices.vdp.vram[1] == 0xCDU &&
            runtime.devices.vdp.vram[2] == 0x12U && runtime.devices.vdp.vram[3] == 0x34U,
        "words transferred in order with unchanged source/target/address progression");
  check(result.kind == GENESIS_STOP && result.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM,
        "the resumed dispatch step's own subsequent frontier is reached normally");
}

// Discriminating case (real routed-access seam, single generated dispatch
// call, no return to genesis_runtime_run in between): proves the
// suspension is enforced at instruction granularity, not merely at a
// generated-dispatch-call boundary. A single synthetic dispatch function
// performs the exact routed VDP CONTROL-port access sequence
// (genesis_route_access -- the identical seam generated C itself uses, not a
// direct struct-field poke) that arms a 2-word GENESIS_VDP_DMA_MEMORY_TO_VRAM
// transfer, then -- WITHOUT returning to genesis_runtime_run -- immediately
// checks, as a synthetic "following instruction" side effect inside that
// SAME dispatch call, whether the DMA is already IDLE and every word has
// already transferred. Against the pre-correction implementation (which only
// drained between generated-dispatch-call boundaries), this check would still
// observe the DMA BUSY at that point, since the arming write and the check
// both occur inside one dispatch call with no intervening
// genesis_runtime_run iteration; this test therefore FAILS against that
// implementation and PASSES only once the drain is a synchronous, per-access
// device-side effect of the arming CONTROL-port write itself.
void arming_write_drains_synchronously_within_same_dispatch_call() {
  GenesisRuntime runtime{};
  // Source words the armed DMA will read from work RAM ($00FF0000), reused
  // unmodified by the shared transfer engine.
  runtime.work_ram[0] = 0xABU; runtime.work_ram[1] = 0xCDU;
  runtime.work_ram[2] = 0x12U; runtime.work_ram[3] = 0x34U;

  bool idle_immediately_after_arming_write = false;
  bool words_already_transferred_immediately_after_arming_write = false;
  int step = 0;
  g_step = [&](GenesisRuntime *r) -> GenesisControlTransfer {
    ++step;
    GenesisRuntimeStop stop{};
    uint32_t value;
    auto control_write = [&](uint32_t word) -> bool {
      value = word;
      return genesis_route_access(r, 0x00C00004U, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, &value,
                                   &stop) == GENESIS_ACCESS_OK;
    };
    // Register #1 bit 0x10 (display enable), required by the arm path.
    if (!control_write(0x8110U)) return own_frontier_stop();
    // Register #15: auto-increment = 2, so the two transferred words land at
    // consecutive VRAM byte offsets 0 and 2 rather than both at offset 0.
    if (!control_write(0x8F02U)) return own_frontier_stop();
    // Registers #19/#20: programmed length = 2 words.
    if (!control_write(0x9302U)) return own_frontier_stop();
    if (!control_write(0x9400U)) return own_frontier_stop();
    // Registers #21/#22/#23: reconstructed DMA source address = $00FF0000,
    // memory-to-VRAM mode (bit7 clear).
    if (!control_write(0x9500U)) return own_frontier_stop();
    if (!control_write(0x9680U)) return own_frontier_stop();
    if (!control_write(0x977FU)) return own_frontier_stop();
    // Two-word address-set command: first word (addr_low14=0, CD1=0/CD0=1),
    // second word (CD5=1 arms the DMA; CD4-CD2=0 selects VRAM WRITE target
    // 0x01 once combined with CD1/CD0 above; addr_high2=0).
    if (!control_write(0x4000U)) return own_frontier_stop();
    // This second write is the exact access that arms the DMA. Under the
    // corrected implementation, genesis_route_access does not return from
    // this call until the armed transfer has already fully drained to
    // GENESIS_VDP_DMA_IDLE.
    if (!control_write(0x0080U)) return own_frontier_stop();
    // Synthetic "following instruction" side effect, still inside this same
    // dispatch call -- never returning to genesis_runtime_run first.
    idle_immediately_after_arming_write = (r->devices.vdp.dma.phase == GENESIS_VDP_DMA_IDLE);
    words_already_transferred_immediately_after_arming_write =
        (r->devices.vdp.vram[0] == 0xABU && r->devices.vdp.vram[1] == 0xCDU &&
         r->devices.vdp.vram[2] == 0x12U && r->devices.vdp.vram[3] == 0x34U &&
         r->devices.vdp.dma.remaining_length == 0U);
    return own_frontier_stop();
  };
  const auto result = genesis_runtime_run(&runtime, trampoline, 128U);
  check(step == 1, "the entire arm sequence and the synthetic following check run in one dispatch call");
  check(idle_immediately_after_arming_write,
        "the DMA is already IDLE immediately after the arming CONTROL-port write returns, "
        "within the SAME dispatch call -- not merely before the NEXT dispatch call");
  check(words_already_transferred_immediately_after_arming_write,
        "every programmed word has already transferred by the time the arming write returns, "
        "within the SAME dispatch call");
  check(result.kind == GENESIS_STOP && result.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM,
        "the dispatch call's own subsequent frontier is reached normally");
}

// Discriminating case (real routed-access seam, ONE routed LONG CONTROL-port
// access): proves the suspension is enforced between the two internal
// sub-transactions of a single LONG CONTROL-port write, not merely once
// after the whole LONG access completes. A LONG write decomposes into two
// sequential WORD bus transactions (high half first). This test arranges the
// VDP control state so the HIGH half is itself the pending second command
// word of an already-latched two-word address-set sequence, and therefore
// arms a 2-word memory-to-VRAM DMA; the LOW half is an ordinary one-word
// register-set command that reprograms register #15 (auto-increment) from 2
// to 4 -- an "observable effect" distinct from the DMA arm itself.
//
// Ordering is proven indirectly but unambiguously through the transfer
// engine's own arithmetic: genesis_vdp_progress_dma reads
// `vdp->auto_increment_value` live at each transferred word, not a snapshot
// taken at arm time. If the DMA is correctly drained to completion BEFORE
// the LOW half's register-#15 write ever runs, both words transfer using the
// increment (2) that was in effect at arm time, landing at VRAM offsets 0
// and 2. If the LOW half's write were (incorrectly) committed before the
// drain runs -- the exact granularity hole this test targets -- the second
// transferred word would instead land at offset 4 (using the new increment,
// 4), and VRAM offset 2 would remain untouched. This test therefore FAILS
// against an implementation that only drains once after the whole LONG
// access completes (or defers the drain to a later dispatch-call boundary)
// and PASSES only once every memory-to-VDP-arming CONTROL-port sub-
// transaction -- including one half of a LONG access -- drains synchronously
// before the NEXT sub-transaction of that same access is allowed to run.
void long_access_drains_between_its_own_two_sub_transactions() {
  GenesisRuntime runtime{};
  runtime.work_ram[0] = 0xABU; runtime.work_ram[1] = 0xCDU;
  runtime.work_ram[2] = 0x12U; runtime.work_ram[3] = 0x34U;

  bool observed_second_word_at_incremented_by_2 = false;
  bool observed_low_half_register_committed = false;
  int step = 0;
  g_step = [&](GenesisRuntime *r) -> GenesisControlTransfer {
    ++step;
    GenesisRuntimeStop stop{};
    uint32_t value;
    auto control_write = [&](uint32_t word) -> bool {
      value = word;
      return genesis_route_access(r, 0x00C00004U, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, &value,
                                   &stop) == GENESIS_ACCESS_OK;
    };
    // Register #1 bit 0x10 (display enable), required by the arm path.
    if (!control_write(0x8110U)) return own_frontier_stop();
    // Register #15: auto-increment = 2 at arm time.
    if (!control_write(0x8F02U)) return own_frontier_stop();
    // Registers #19/#20: programmed length = 2 words.
    if (!control_write(0x9302U)) return own_frontier_stop();
    if (!control_write(0x9400U)) return own_frontier_stop();
    // Registers #21/#22/#23: reconstructed DMA source address = $00FF0000,
    // memory-to-VRAM mode (bit7 clear).
    if (!control_write(0x9500U)) return own_frontier_stop();
    if (!control_write(0x9680U)) return own_frontier_stop();
    if (!control_write(0x977FU)) return own_frontier_stop();
    // Latch the FIRST word of the two-word address-set command as its own,
    // separate, already-completed prior access (addr_low14=0, CD1=0/CD0=1).
    // This is the reviewer-specified precondition: control_port_awaiting_
    // second_word is already true, with a valid first command word already
    // latched, BEFORE the LONG access below ever begins.
    if (!control_write(0x4000U)) return own_frontier_stop();
    check(r->devices.vdp.control_port_awaiting_second_word != 0,
          "precondition: the second command word is pending before the LONG access begins");
    // ONE routed LONG CONTROL-port write. High half = 0x0080 (CD5=1 arms the
    // DMA; CD4-CD2=0 selects VRAM WRITE target 0x01 once combined with
    // CD1/CD0 above; addr_high2=0) -- the pending second command word, so
    // the DMA arms as a direct effect of the HIGH half alone. Low half =
    // 0x8F04 (register-set: register #15 = 4) -- an ordinary, independently
    // observable CONTROL-port effect with no DMA involvement of its own.
    value = (UINT32_C(0x0080) << 16) | UINT32_C(0x8F04);
    const bool ok = genesis_route_access(r, 0x00C00004U, GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE,
                                          &value, &stop) == GENESIS_ACCESS_OK;
    if (!ok) return own_frontier_stop();
    observed_second_word_at_incremented_by_2 =
        (r->devices.vdp.dma.phase == GENESIS_VDP_DMA_IDLE && r->devices.vdp.dma.remaining_length == 0U &&
         r->devices.vdp.vram[0] == 0xABU && r->devices.vdp.vram[1] == 0xCDU &&
         r->devices.vdp.vram[2] == 0x12U && r->devices.vdp.vram[3] == 0x34U &&
         r->devices.vdp.vram[4] == 0U && r->devices.vdp.vram[5] == 0U);
    observed_low_half_register_committed =
        (r->devices.vdp.registers[15] == 4U && r->devices.vdp.auto_increment_value == 4U);
    return own_frontier_stop();
  };
  const auto result = genesis_runtime_run(&runtime, trampoline, 128U);
  check(step == 1, "the entire arm sequence and the single LONG access run in one dispatch call");
  check(observed_second_word_at_incremented_by_2,
        "the DMA transfer completed using the increment in effect at arm time (offsets 0/2), "
        "proving it ran to completion before the LOW half's own register write");
  check(observed_low_half_register_committed,
        "the LOW half's own observable CONTROL-port effect still committed, just after the drain");
  check(result.kind == GENESIS_STOP && result.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM,
        "the dispatch call's own subsequent frontier is reached normally");
}

// Case: an unroutable DMA source (the existing genesis_vdp_dma_source_is_routable
// check already rejects any address that is neither below $00400000 nor inside
// work RAM) fails closed deterministically during the automatic drain, and the
// later generated CPU instruction is never executed.
void unroutable_dma_source_fails_closed_without_resuming_dispatch() {
  GenesisRuntime runtime{};
  int step = 0;
  g_step = [&](GenesisRuntime *r) -> GenesisControlTransfer {
    ++step;
    if (step == 1) {
      r->devices.vdp.dma.phase = GENESIS_VDP_DMA_BUSY;
      r->devices.vdp.dma.kind = GENESIS_VDP_DMA_MEMORY_TO_VRAM;
      r->devices.vdp.dma.source_address = 0x00A00000U; /* Z80 bus region: unroutable DMA source */
      r->devices.vdp.dma.remaining_length = 5U;
      r->devices.vdp.dma.write_target_code = 0x01U;
      return continue_at(0x10U);
    }
    /* Reaching this step at all is the failure this test guards against. */
    return own_frontier_stop();
  };
  const auto result = genesis_runtime_run(&runtime, trampoline, 128U);
  check(step == 1, "the later generated CPU instruction never executes after an unroutable DMA source");
  check(result.kind == GENESIS_STOP &&
            result.stop.stop_class == GENESIS_STOP_UNSUPPORTED_MEMORY_REGION,
        "an unroutable DMA source fails closed deterministically during the automatic drain");
}

// Regression: GENESIS_VDP_DMA_VRAM_FILL is untouched by the new drain -- its
// own completion remains exclusively driven by the CPU's DATA-port write, so
// the following generated dispatch step must still run normally while the
// FILL DMA remains BUSY (draining it here would never terminate).
void vram_fill_dma_write_routing_is_unchanged() {
  GenesisRuntime runtime{};
  int step = 0;
  g_step = [&](GenesisRuntime *r) -> GenesisControlTransfer {
    ++step;
    if (step == 1) {
      r->devices.vdp.dma.phase = GENESIS_VDP_DMA_BUSY;
      r->devices.vdp.dma.kind = GENESIS_VDP_DMA_VRAM_FILL;
      r->devices.vdp.dma.fill_byte_count = 4U;
      return continue_at(0x10U);
    }
    return own_frontier_stop();
  };
  const auto result = genesis_runtime_run(&runtime, trampoline, 128U);
  check(step == 2, "a FILL-mode DMA does not block the following generated dispatch step");
  check(runtime.devices.vdp.dma.phase == GENESIS_VDP_DMA_BUSY &&
            runtime.devices.vdp.dma.kind == GENESIS_VDP_DMA_VRAM_FILL,
        "FILL DMA state is unaffected by the new memory-to-VDP drain");
  check(result.kind == GENESIS_STOP && result.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM,
        "ordinary dispatch progression continues normally for a FILL DMA");
}

// Regression: ordinary (non-DMA) generated CPU dispatch behaviour is
// unaffected when no memory-to-VDP DMA is ever armed.
void ordinary_execution_unaffected_when_no_dma_armed() {
  GenesisRuntime runtime{};
  int step = 0;
  g_step = [&](GenesisRuntime *r) -> GenesisControlTransfer {
    (void)r;
    ++step;
    if (step < 3) return continue_at(0x10U);
    return own_frontier_stop();
  };
  const auto result = genesis_runtime_run(&runtime, trampoline, 128U);
  check(step == 3, "ordinary dispatch stepping is unaffected when no DMA is armed");
  check(result.kind == GENESIS_STOP && result.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM,
        "ordinary non-DMA execution reaches its own frontier unchanged");
}

}  // namespace

int main() {
  multi_word_dma_drains_before_next_dispatch_step();
  arming_write_drains_synchronously_within_same_dispatch_call();
  long_access_drains_between_its_own_two_sub_transactions();
  unroutable_dma_source_fails_closed_without_resuming_dispatch();
  vram_fill_dma_write_routing_is_unchanged();
  ordinary_execution_unaffected_when_no_dma_armed();
  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("OK\n");
  return 0;
}
