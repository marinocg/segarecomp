// SEG-020-T003: single bounded typed execution history. Project-authored
// synthetic values only.
#include "runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char *what) {
  if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

// Captures the ephemeral channel (both lines) via a portable temporary file.
std::string dump(const GenesisRuntime &runtime, GenesisControlTransferKind kind = GENESIS_RUNNER_RESOURCE_LIMIT) {
  GenesisControlTransfer result{};
  result.kind = kind;
  FILE *file = std::tmpfile();
  check(file != nullptr, "tmpfile");
  if (file == nullptr) return {};
  check(genesis_write_ephemeral_pc_history(file, &runtime, &result) == 0, "writer succeeds");
  std::rewind(file);
  std::string out;
  char chunk[4096];
  size_t got;
  while ((got = std::fread(chunk, 1, sizeof chunk, file)) > 0) out.append(chunk, got);
  std::fclose(file);
  return out;
}

size_t count_of(const std::string &text, const std::string &needle) {
  size_t n = 0;
  for (size_t pos = 0; (pos = text.find(needle, pos)) != std::string::npos; ++pos) ++n;
  return n;
}

GenesisControlTransfer retire(GenesisRuntime *r, uint32_t pc, uint32_t fall, GenesisHistoryTransferKind kind,
                              uint32_t next) {
  return genesis_runtime_retire_m68k_instruction_at(r, pc, fall, kind, 4U, next);
}

void drive_mixed(GenesisRuntime *r, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t pc = 0x1000U + i * 2U;
    const bool taken = i % 3U == 0U;
    retire(r, pc, pc + 2U, taken ? GENESIS_HISTORY_TRANSFER_DIRECT : GENESIS_HISTORY_TRANSFER_NONE,
           taken ? 0x2000U + i : pc + 2U);
  }
}

GenesisRuntime stop_retirement_fixture() {
  GenesisRuntime runtime{};
  runtime.d[0] = 0x11223344U;
  runtime.a[7] = SEGARECOMP_GENESIS_WORK_RAM_BEGIN + 0x1000U;
  runtime.sr = 0x2000U;
  runtime.pc = 0x100U;
  runtime.devices.vdp.registers[1] = 0x20U;
  runtime.devices.interrupt.vblank_pending = 1U;
  runtime.devices.interrupt.vblank_transition_count = 3U;
  runtime.scheduler.master_ticks = GENESIS_NTSC_VBLANK_ONSET_TICK - 28U;
  runtime.irq6_handler_entry = 0x800U;
  runtime.irq6_handler_present = 1U;
  runtime.execution_history.detail_enabled = 1U;
  runtime.m68k_checkpoint.enabled = 1U;
  runtime.m68k_checkpoint.effect_count = 1U;
  runtime.m68k_checkpoint.effects[0].kind = GENESIS_M68K_EFFECT_WRITE;
  runtime.device_checkpoint.enabled = 1U;
  runtime.device_checkpoint.event_count = 1U;
  runtime.device_checkpoint.events[0].kind = GENESIS_DEVICE_EVENT_WRITE;
  return runtime;
}

void check_invalid_stop_retirement_is_atomic(bool history_aware, const GenesisControlTransfer *pending_stop,
                                             const char *what) {
  GenesisRuntime runtime = stop_retirement_fixture();
  unsigned char before[sizeof(runtime)];
  std::memcpy(before, &runtime, sizeof(runtime));
  GenesisControlTransfer result{};
  if (history_aware) {
    result = genesis_runtime_retire_m68k_instruction_at_before_stop(
        &runtime, 0x100U, 0x102U, GENESIS_HISTORY_TRANSFER_DIRECT, 4U, 0x200U, pending_stop);
  } else {
    result = genesis_runtime_retire_m68k_instruction_before_stop(&runtime, 4U, 0x200U, pending_stop);
  }
  check(result.kind == GENESIS_STOP &&
            result.stop.stop_class == GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY &&
            result.stop.diagnostic_category == GENESIS_DIAG_INTERNAL_DISPATCH_INCONSISTENCY,
        what);
  check(std::memcmp(before, &runtime, sizeof(runtime)) == 0, what);
}
}  // namespace

int main() {
  // 1+2+7: capacity/overwrite and deterministic mixed ordering across wrap.
  GenesisRuntime a{};
  a.execution_history.detail_enabled = 1U;
  const uint32_t n = GENESIS_EXECUTION_HISTORY_CAPACITY;  // 256 instrs -> >256 events
  drive_mixed(&a, n);
  const GenesisExecutionHistory &h = a.execution_history;
  check(h.total_recorded > GENESIS_EXECUTION_HISTORY_CAPACITY, "setup: ring overflowed");
  const std::string text = dump(a);
  const size_t second_line = text.find('\n') + 1U;
  const std::string typed = text.substr(second_line);
  check(count_of(typed, "\"k\"") == GENESIS_EXECUTION_HISTORY_CAPACITY, "typed dump bounded by capacity");
  GenesisRuntime b{};
  b.execution_history.detail_enabled = 1U;
  drive_mixed(&b, n);
  check(dump(b) == text, "identical runs produce byte-identical history");
  // Newest event is the last retirement; ordering is oldest->newest.
  check(typed.find("\"b\":" + std::to_string(n - 1U) + ",\"k\":\"retired\"") != std::string::npos, "newest retire kept");
  check(typed.rfind("[{", 0) == 0 && typed.find("\"b\":0,") == std::string::npos, "oldest overwritten");

  // 4+5: retired PC distinct from next PC; taken transfer minimal event; not-taken/none emits none.
  GenesisRuntime c{};
  c.execution_history.detail_enabled = 1U;
  retire(&c, 0x100U, 0x102U, GENESIS_HISTORY_TRANSFER_DIRECT, 0x200U);
  retire(&c, 0x200U, 0x202U, GENESIS_HISTORY_TRANSFER_CALL, 0x202U);  // fallthrough: not taken
  retire(&c, 0x202U, 0x204U, GENESIS_HISTORY_TRANSFER_RETURN, 0x300U);
  const std::string ct = dump(c, GENESIS_STOP);
  check(ct == "[]\n[{\"b\":0,\"k\":\"transfer\",\"t\":1,\"next\":\"0x00000200\"},"
              "{\"b\":0,\"k\":\"retired\",\"pc\":\"0x00000100\",\"next\":\"0x00000200\"},"
              "{\"b\":1,\"k\":\"retired\",\"pc\":\"0x00000200\",\"next\":\"0x00000202\"},"
              "{\"b\":2,\"k\":\"transfer\",\"t\":4,\"next\":\"0x00000300\"},"
              "{\"b\":2,\"k\":\"retired\",\"pc\":\"0x00000202\",\"next\":\"0x00000300\"}]\n",
        "retired pc/next pc and taken-transfer events exact");

  // 6: device-visible read and write: width/direction/region only, no values.
  GenesisRuntime d{};
  d.execution_history.detail_enabled = 1U;
  uint32_t value = 0U;
  GenesisRuntimeStop stop{};
  const auto rd = genesis_route_access(&d, 0x00C00004U, GENESIS_ACCESS_WORD, GENESIS_ACCESS_READ, &value, &stop);
  value = 0x1234U;
  const auto wr = genesis_route_access(&d, 0x00C00004U, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, &value, &stop);
  uint32_t ram = 0xDEADBEEFU;
  (void)genesis_route_access(&d, 0x00FF0000U, GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE, &ram, &stop);
  const std::string dt = dump(d, GENESIS_STOP);
  size_t expected = (rd == GENESIS_ACCESS_OK ? 1U : 0U) + (wr == GENESIS_ACCESS_OK ? 1U : 0U);
  check(expected >= 1U, "setup: at least one synthetic VDP access is admitted");
  check(count_of(dt, "\"k\":\"access\"") == expected, "one access event per admitted device access; RAM not recorded");
  if (rd == GENESIS_ACCESS_OK) check(dt.find("{\"b\":0,\"k\":\"access\",\"bk\":2,\"r\":4,\"w\":2,\"d\":0}") != std::string::npos, "VDP read event");
  if (wr == GENESIS_ACCESS_OK) check(dt.find("\"bk\":3,\"r\":4,\"w\":2,\"d\":1}") != std::string::npos, "VDP write event");
  check(dt.find("1234") == std::string::npos && dt.find("deadbeef") == std::string::npos, "no values recorded");

  // Bus kind: stack accesses keep their existing kind; generic seam is data; mismatches fail closed.
  GenesisRuntime k{};
  k.execution_history.detail_enabled = 1U;
  uint32_t kv = 0U;
  GenesisRuntimeStop ks{};
  const auto sw = genesis_route_access_bus(&k, GENESIS_BUS_STACK_WRITE, 0x00C00004U, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, &kv, &ks);
  const auto sr = genesis_route_access_bus(&k, GENESIS_BUS_STACK_READ, 0x00C00004U, GENESIS_ACCESS_WORD, GENESIS_ACCESS_READ, &kv, &ks);
  const auto bad = genesis_route_access_bus(&k, GENESIS_BUS_STACK_READ, 0x00C00004U, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, &kv, &ks);
  check(bad == GENESIS_ACCESS_FAIL, "bus kind/direction mismatch fails closed");
  const std::string kt = dump(k, GENESIS_STOP);
  if (sw == GENESIS_ACCESS_OK) check(kt.find("\"bk\":5,\"r\":4,\"w\":2,\"d\":1}") != std::string::npos, "stack write kind");
  if (sr == GENESIS_ACCESS_OK) check(kt.find("\"bk\":4,\"r\":4,\"w\":2,\"d\":0}") != std::string::npos, "stack read kind");
  check(count_of(kt, "\"k\":\"access\"") == (sw == GENESIS_ACCESS_OK ? 1U : 0U) + (sr == GENESIS_ACCESS_OK ? 1U : 0U), "failed access not recorded");
  check(rd == GENESIS_ACCESS_OK || wr == GENESIS_ACCESS_OK, "setup: data accesses admitted");

  // 3: disabled does not alter results; detail events absent, and the typed line is absent.
  GenesisRuntime off{};
  GenesisRuntime on{};
  on.execution_history.detail_enabled = 1U;
  drive_mixed(&off, 40U);
  drive_mixed(&on, 40U);
  check(off.pc == on.pc && off.sr == on.sr, "execution state unchanged by history");
  check(off.execution_history.total_recorded == 0U, "disabled records nothing");
  check(dump(off) == "[]\n", "disabled emits only the pc-history line");

  // 8: the recent-PC view is a projection of the same ring (dispatch events).
  GenesisRuntime e{};
  auto step = [](GenesisRuntime *r) -> GenesisControlTransfer {
    GenesisControlTransfer t{};
    t.kind = GENESIS_CONTINUE_AT_PC;
    t.next_pc = r->pc + 4U;
    return t;
  };
  e.pc = 0x4000U;
  genesis_runtime_run(&e, step, 100U);
  uint32_t projected[GENESIS_RECENT_PC_HISTORY_CAPACITY];
  check(genesis_recent_pc_history_project(&e, projected) == GENESIS_RECENT_PC_HISTORY_CAPACITY, "projection capped at 64");
  check(projected[63] == 0x4000U + 99U * 4U && projected[0] == 0x4000U + 36U * 4U, "projection is newest 64 dispatches");
  check(e.execution_history.total_recorded == 100U, "dispatches live in the single ring");
  check(dump(e).find('\n') == dump(e).rfind('\n'), "dispatch-only run emits only the pc line");

  // Exception entry (VBlank IRQ6 admission at a retirement boundary) is a TRANSFER event.
  GenesisRuntime x{};
  x.execution_history.detail_enabled = 1U;
  x.pc = 0x100U;
  x.sr = 0x2000U;
  x.a[7] = SEGARECOMP_GENESIS_WORK_RAM_BEGIN + 0x1000U;
  x.devices.vdp.registers[1] = 0x20U;
  x.irq6_handler_entry = 0x800U;
  x.irq6_handler_present = 1U;
  x.scheduler.master_ticks = GENESIS_NTSC_VBLANK_ONSET_TICK - 28U;
  const auto xr = retire(&x, 0x100U, 0x102U, GENESIS_HISTORY_TRANSFER_NONE, 0x102U);
  check(xr.kind == GENESIS_CONTINUE_AT_PC && xr.next_pc == 0x800U, "setup: IRQ6 admitted");
  check(dump(x, GENESIS_STOP).find("{\"b\":1,\"k\":\"transfer\",\"t\":5,\"next\":\"0x00000800\"}") != std::string::npos,
        "exception entry recorded at the following boundary ordinal");

  // Stop-aware retirement is a strict producer contract. NULL and non-stop
  // pointers fail before PC/timing/device/checkpoint/history mutation, even
  // when an IRQ is otherwise immediately admissible.
  GenesisControlTransfer not_a_stop{};
  not_a_stop.kind = GENESIS_CONTINUE_AT_PC;
  not_a_stop.next_pc = 0x400U;
  check_invalid_stop_retirement_is_atomic(false, nullptr, "plain stop retirement rejects NULL atomically");
  check_invalid_stop_retirement_is_atomic(false, &not_a_stop, "plain stop retirement rejects non-stop atomically");
  check_invalid_stop_retirement_is_atomic(true, nullptr, "history stop retirement rejects NULL atomically");
  check_invalid_stop_retirement_is_atomic(true, &not_a_stop, "history stop retirement rejects non-stop atomically");

  // A valid producer stop still retires normally while deferring an otherwise
  // admissible IRQ: time/checkpoint/history advance, but no frame is built and
  // the pending interrupt is not consumed.
  GenesisRuntime valid = stop_retirement_fixture();
  GenesisControlTransfer producer_stop{};
  producer_stop.kind = GENESIS_STOP;
  producer_stop.stop.stop_class = GENESIS_STOP_KNOWN_BUT_UNEMITTED_TARGET;
  producer_stop.stop.diagnostic_category = GENESIS_DIAG_KNOWN_BUT_UNEMITTED_TARGET;
  const uint32_t valid_a7 = valid.a[7];
  const auto valid_result = genesis_runtime_retire_m68k_instruction_at_before_stop(
      &valid, 0x100U, 0x102U, GENESIS_HISTORY_TRANSFER_NONE, 4U, 0x102U, &producer_stop);
  check(valid_result.kind == GENESIS_STOP &&
            valid_result.stop.stop_class == GENESIS_STOP_KNOWN_BUT_UNEMITTED_TARGET,
        "valid producer stop is preserved");
  check(valid.pc == 0x102U && valid.scheduler.master_ticks == GENESIS_NTSC_VBLANK_ONSET_TICK,
        "valid producer stop retires PC and timing");
  check(valid.a[7] == valid_a7 && valid.devices.interrupt.vblank_pending == 1U,
        "valid producer stop defers admissible IRQ without a frame or pending-bit consumption");
  check(valid.execution_history.retired_count == 1U && valid.execution_history.total_recorded == 1U,
        "valid producer stop records exactly one retirement");
  check(valid.m68k_checkpoint.valid == 1U && valid.m68k_checkpoint.pc == 0x102U,
        "valid producer stop finalizes its checkpoint");

  GenesisRuntime timing_failure = stop_retirement_fixture();
  const auto timing_result = genesis_runtime_retire_m68k_instruction_before_stop(
      &timing_failure, 0U, 0x102U, &producer_stop);
  check(timing_result.kind == GENESIS_STOP &&
            timing_result.stop.diagnostic_category == GENESIS_DIAG_UNACCOUNTED_INSTRUCTION_TIMING,
        "scheduler failure retains precedence over a valid producer stop");

  if (failures != 0) return 1;
  std::puts("genesis_execution_history_tests passed");
  return 0;
}
