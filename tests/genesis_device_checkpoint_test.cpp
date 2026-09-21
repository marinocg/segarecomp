// SEG-020-T006: opt-in Genesis device checkpoint. Project-authored synthetic values only.
// Demonstrates CPU-equivalent boundaries whose device events/state differ, bounded storage,
// and that a diagnostics-off runtime never reads or writes the checkpoint.
#include "runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
int failures = 0;
void check(bool ok, const char *what) {
  if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

GenesisRuntime make(bool enabled) {
  GenesisRuntime r{};
  r.m68k_checkpoint.enabled = enabled ? 1U : 0U;
  r.device_checkpoint.enabled = enabled ? 1U : 0U;
  r.sr = 0x2700U;
  r.a[7] = 0xFFFF00U;
  return r;
}

void write(GenesisRuntime *r, uint32_t addr, GenesisAccessWidth w, uint32_t v) {
  GenesisRuntimeStop stop{};
  check(genesis_route_access(r, addr, w, GENESIS_ACCESS_WRITE, &v, &stop) == GENESIS_ACCESS_OK, "write routed");
}

void retire(GenesisRuntime *r) { genesis_runtime_retire_m68k_instruction(r, 4U, 0x202U); }

// Boundary 1: VDP register set (control-port command) + a data-port write to VRAM.
void program(GenesisRuntime *r) {
  write(r, 0xC00004U, GENESIS_ACCESS_WORD, 0x8F02U);  // register 15 (auto-increment) = 2
  write(r, 0xC00004U, GENESIS_ACCESS_WORD, 0x4000U);  // VRAM write command, first word
  write(r, 0xC00004U, GENESIS_ACCESS_WORD, 0x0000U);  // second word
  write(r, 0xC00000U, GENESIS_ACCESS_WORD, 0x1234U);  // data port
}

std::string detail(const GenesisRuntime &r, bool device) {
  FILE *f = std::tmpfile();  // portable capture (no POSIX-only open_memstream)
  check(f != nullptr, "tmpfile");
  if (f == nullptr) return {};
  if (device) genesis_device_checkpoint_write_detail(f, &r); else genesis_m68k_checkpoint_write_detail(f, &r);
  std::rewind(f);
  std::string out;
  char chunk[256];
  size_t n;
  while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) out.append(chunk, n);
  std::fclose(f);
  return out;
}
}  // namespace

int main() {
  GenesisRuntime base = make(true);
  program(&base);
  retire(&base);
  check(base.device_checkpoint.valid && base.device_checkpoint.boundary == 1U, "boundary recorded");
  check(base.device_checkpoint.last_event_count == 4U, "four VDP command events observed");
  check(base.device_checkpoint.last_events[0].kind == GENESIS_DEVICE_EVENT_WRITE &&
            base.device_checkpoint.last_events[0].region == GENESIS_HISTORY_REGION_VDP, "VDP region class");

  // Determinism: identical runs give identical digests and detail bytes.
  GenesisRuntime again = make(true);
  program(&again);
  retire(&again);
  check(again.device_checkpoint.digest == base.device_checkpoint.digest, "stable digest");
  check(detail(again, true) == detail(base, true), "stable detail bytes");
  const char *component = "x";
  check(genesis_device_checkpoint_compare(&base.device_checkpoint, &again.device_checkpoint, &component) ==
            GENESIS_DEVICE_CHECKPOINT_EQUAL, "identical boundaries equal");

  // Case A: device *state* evolution differs (VRAM byte corrupted after the same commands) while
  // the M68k checkpoint is identical -> first differing domain is the device, component vdp_vram.
  GenesisRuntime state_bad = make(true);
  program(&state_bad);
  state_bad.devices.vdp.vram[0] ^= 0x01U;
  state_bad.device_checkpoint.mem_dirty = 1U;
  retire(&state_bad);
  check(genesis_m68k_checkpoint_compare(&base.m68k_checkpoint, &state_bad.m68k_checkpoint) ==
            GENESIS_M68K_CHECKPOINT_EQUAL, "state case: CPU checkpoints equal");
  check(genesis_device_checkpoint_compare(&base.device_checkpoint, &state_bad.device_checkpoint, &component) ==
            GENESIS_DEVICE_CHECKPOINT_STATE_DIFFERENT, "state case: device state differs");
  check(std::strcmp(component, "vdp_vram") == 0, "state case: component named");

  // Case B: DMA state evolution differs (register-level) -> vdp_dma component.
  GenesisRuntime dma_bad = make(true);
  program(&dma_bad);
  dma_bad.devices.vdp.dma.remaining_length = 3U;
  retire(&dma_bad);
  check(genesis_device_checkpoint_compare(&base.device_checkpoint, &dma_bad.device_checkpoint, &component) ==
                GENESIS_DEVICE_CHECKPOINT_STATE_DIFFERENT && std::strcmp(component, "vdp_dma") == 0,
        "dma case: component named");

  // Case C: an interrupt event differs (VBLANK rising edge + admission) with CPU state equal.
  GenesisRuntime irq_bad = make(true);
  program(&irq_bad);
  irq_bad.devices.interrupt.vblank_pending = 1U;
  ++irq_bad.devices.interrupt.vblank_transition_count;
  retire(&irq_bad);
  check(genesis_m68k_checkpoint_compare(&base.m68k_checkpoint, &irq_bad.m68k_checkpoint) ==
            GENESIS_M68K_CHECKPOINT_EQUAL, "irq case: CPU checkpoints equal (IRQ masked)");
  check(genesis_device_checkpoint_compare(&base.device_checkpoint, &irq_bad.device_checkpoint, &component) ==
                GENESIS_DEVICE_CHECKPOINT_EVENT_DIFFERENT && std::strcmp(component, "events") == 0,
        "irq case: device event differs");
  check(irq_bad.device_checkpoint.last_events[irq_bad.device_checkpoint.last_event_count - 1U].kind ==
            GENESIS_DEVICE_EVENT_VBLANK_RAISE, "vblank raise event recorded");

  // Case D: wrong command value -> both domains see it; the device event names the command.
  GenesisRuntime cmd_bad = make(true);
  write(&cmd_bad, 0xC00004U, GENESIS_ACCESS_WORD, 0x8F04U);
  write(&cmd_bad, 0xC00004U, GENESIS_ACCESS_WORD, 0x4000U);
  write(&cmd_bad, 0xC00004U, GENESIS_ACCESS_WORD, 0x0000U);
  write(&cmd_bad, 0xC00000U, GENESIS_ACCESS_WORD, 0x1234U);
  retire(&cmd_bad);
  check(genesis_m68k_checkpoint_compare(&base.m68k_checkpoint, &cmd_bad.m68k_checkpoint) ==
            GENESIS_M68K_CHECKPOINT_DIFFERENT, "command case: CPU write effect differs first");
  check(genesis_device_checkpoint_compare(&base.device_checkpoint, &cmd_bad.device_checkpoint, &component) ==
            GENESIS_DEVICE_CHECKPOINT_EVENT_DIFFERENT, "command case: device event differs");

  // Non-device (work RAM) writes are not device events.
  GenesisRuntime ram = make(true);
  write(&ram, 0xFF0100U, GENESIS_ACCESS_WORD, 0x1U);
  retire(&ram);
  check(ram.device_checkpoint.last_event_count == 0U, "work RAM write is not a device event");

  // Bounded storage: more events than capacity -> unsupported, never equal, no overflow.
  GenesisRuntime flood = make(true);
  for (unsigned i = 0; i < GENESIS_DEVICE_CHECKPOINT_EVENT_CAPACITY + 5U; ++i)
    write(&flood, 0xC00004U, GENESIS_ACCESS_WORD, 0x8F02U);
  retire(&flood);
  check(flood.device_checkpoint.last_unsupported == 1U &&
            flood.device_checkpoint.last_event_count == GENESIS_DEVICE_CHECKPOINT_EVENT_CAPACITY, "flood is bounded");
  check(genesis_device_checkpoint_compare(&flood.device_checkpoint, &flood.device_checkpoint, &component) ==
            GENESIS_DEVICE_CHECKPOINT_UNSUPPORTED, "unsupported never equal");
  check(sizeof(base.device_checkpoint.events) / sizeof(base.device_checkpoint.events[0]) ==
            GENESIS_DEVICE_CHECKPOINT_EVENT_CAPACITY, "compile-time capacity");
  // Storage does not grow with executed boundaries.
  for (int i = 0; i < 1000; ++i) retire(&base);
  check(base.device_checkpoint.boundary == 1001U, "boundary count advances without growth");

  // Diagnostics off: never read or written.
  GenesisRuntime off = make(false);
  program(&off);
  retire(&off);
  GenesisDeviceCheckpoint zero{};
  check(std::memcmp(&off.device_checkpoint, &zero, sizeof zero) == 0, "disabled checkpoint untouched");
  check(detail(off, true) == "{\"device_checkpoint\":null}\n", "disabled detail is null");

  if (failures == 0) std::printf("genesis device checkpoint OK\n");
  return failures == 0 ? 0 : 1;
}
