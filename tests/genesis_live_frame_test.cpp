// SEG-007-T255: live frame production at deterministic virtual frame boundaries.
// Project-authored synthetic state only; no ROM, no host clock.
#include "runtime.h"
#include "vdp_render.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
int failures = 0;
void check(bool value, const char *message) { if (!value) { std::printf("FAIL: %s\n", message); ++failures; } }

constexpr uint32_t kRamBegin = 0xFF0000u;
constexpr uint64_t kOnsetMinus4Cycles = GENESIS_NTSC_VBLANK_ONSET_TICK - 28u;

void renderable_registers(uint16_t regs[GENESIS_VDP_REGISTER_COUNT]) {
  std::memset(regs, 0, sizeof(uint16_t) * GENESIS_VDP_REGISTER_COUNT);
  regs[1] = 0x04; regs[2] = 0x30; regs[4] = 0x04; regs[5] = 0x28;
  regs[11] = 0x04; regs[12] = 0x81; regs[16] = 0x01;
}

void setup(GenesisRuntime &r, bool ie0) {
  r.pc = 0x200u; r.sr = 0x2000u;
  renderable_registers(r.devices.vdp.registers);
  if (ie0) r.devices.vdp.registers[1] |= 0x20u;
  r.devices.vdp.cram[0] = 0x0Eu; r.devices.vdp.cram[1] = 0x00u;  // non-trivial backdrop
  r.scheduler.master_ticks = kOnsetMinus4Cycles;
}

struct Observed { GenesisFrameArtifact frame; GenesisLiveFrameObserver observer; };
void attach(Observed &o, GenesisRuntime &r) {
  std::memset(&o, 0, sizeof(o));
  o.observer.producer = genesis_vdp_produce_frame;
  o.observer.latest = &o.frame;
  r.live_frame_observer = &o.observer;
}

void no_boundary_no_frame() {
  GenesisRuntime r{}; Observed o; setup(r, true); attach(o, r);
  r.scheduler.master_ticks = kOnsetMinus4Cycles - 7u;
  (void)genesis_runtime_retire_m68k_instruction(&r, 4u, 0x200u);
  check(o.observer.sequence == 0u && !genesis_frame_artifact_is_populated(&o.frame), "no boundary -> no frame");
}

void exact_onset_one_frame_using_current_vdp_state() {
  GenesisRuntime r{}; Observed o; setup(r, true); attach(o, r);
  (void)genesis_runtime_retire_m68k_instruction(&r, 4u, 0x200u);
  check(o.observer.sequence == 1u, "exact onset publishes one frame");
  GenesisFrameArtifact expect; std::memset(&expect, 0, sizeof(expect));
  check(genesis_vdp_produce_frame(r.devices.vdp.vram, r.devices.vdp.vsram, r.devices.vdp.cram,
                                  r.devices.vdp.registers, &expect) == 0, "reference render works");
  check(std::memcmp(&expect, &o.frame, sizeof(expect)) == 0, "frame equals render of current VDP state");
  check(genesis_frame_artifact_is_populated(&o.frame), "published frame populated");
  (void)genesis_runtime_retire_m68k_instruction(&r, 4u, 0x202u);
  check(o.observer.sequence == 1u, "post-onset retirement publishes nothing");
}

void produces_with_ie0_disabled_and_irq_masked() {
  GenesisRuntime a{}; Observed oa; setup(a, false); attach(oa, a);
  (void)genesis_runtime_retire_m68k_instruction(&a, 4u, 0x200u);
  check(oa.observer.sequence == 1u && a.devices.interrupt.vblank_transition_count == 0u, "IE0 disabled still produces");
  GenesisRuntime b{}; Observed ob; setup(b, true); attach(ob, b);
  b.sr = 0x2700u; b.irq6_handler_present = 1u; b.irq6_handler_entry = 0x300u; b.a[7] = kRamBegin + 0x8000u;
  const GenesisControlTransfer t = genesis_runtime_retire_m68k_instruction(&b, 4u, 0x200u);
  check(ob.observer.sequence == 1u && t.next_pc == 0x200u && b.devices.interrupt.vblank_pending == 1u,
        "SR-masked IRQ6 still produces; IRQ stays pending");
}

void irq_admission_does_not_select_frame_existence() {
  GenesisRuntime r{}; Observed o; setup(r, true); attach(o, r);
  r.irq6_handler_present = 1u; r.irq6_handler_entry = 0x300u; r.a[7] = kRamBegin + 0x8000u;
  r.scheduler.master_ticks = kOnsetMinus4Cycles - 7u;
  (void)genesis_runtime_retire_m68k_instruction(&r, 4u, 0x200u);
  check(o.observer.sequence == 0u, "no frame before boundary despite armed IRQ6 setup");
  const GenesisControlTransfer t = genesis_runtime_retire_m68k_instruction(&r, 4u, 0x202u);
  check(t.next_pc == 0x300u && o.observer.sequence == 1u, "boundary admits IRQ6 and produces exactly one frame");
  (void)genesis_runtime_retire_m68k_instruction(&r, 4u, 0x300u);  // handler / pending cleared
  check(o.observer.sequence == 1u, "IRQ admission/entry does not create frames");
}

int failing_producer(const uint8_t *, const uint8_t *, const uint8_t *, const uint16_t *, GenesisFrameArtifact *out) {
  out->pixels[0] = 0xAAu; return 1;
}

void unrenderable_publishes_nothing() {
  GenesisRuntime r{}; Observed o; setup(r, true); attach(o, r);
  std::memset(o.frame.pixels, 0x55, 16);
  const GenesisFrameArtifact before = o.frame;
  std::memset(r.devices.vdp.registers, 0, sizeof(r.devices.vdp.registers));
  r.devices.vdp.registers[1] = 0x20u;  // Mode 5 clear: outside the bound surface
  (void)genesis_runtime_retire_m68k_instruction(&r, 4u, 0x200u);
  check(o.observer.sequence == 0u && std::memcmp(&before, &o.frame, sizeof(before)) == 0,
        "unrenderable state publishes nothing and keeps artifact");
  GenesisRuntime f{}; Observed of; setup(f, true); attach(of, f); of.observer.producer = failing_producer;
  (void)genesis_runtime_retire_m68k_instruction(&f, 4u, 0x200u);
  check(of.observer.sequence == 0u && of.frame.pixels[0] == 0u, "failed producer leaves artifact untouched (atomic)");
}

void observer_is_non_semantic() {
  GenesisRuntime plain{}; setup(plain, true);
  GenesisRuntime watched{}; Observed o; setup(watched, true); attach(o, watched);
  watched.live_frame_observer = &o.observer;
  for (uint32_t i = 0; i < 4u; ++i) {
    const GenesisControlTransfer a = genesis_runtime_retire_m68k_instruction(&plain, 4u + i, 0x200u + 2u * i);
    const GenesisControlTransfer b = genesis_runtime_retire_m68k_instruction(&watched, 4u + i, 0x200u + 2u * i);
    check(a.kind == b.kind && a.next_pc == b.next_pc, "control transfer identical with observer");
  }
  watched.live_frame_observer = nullptr;
  check(std::memcmp(&plain, &watched, sizeof(plain)) == 0, "guest CPU/device/T253 state byte-identical");
  check(o.observer.sequence == 1u, "observer saw the boundary");
}

void multiple_boundaries_latest_frame_and_sequence() {
  GenesisRuntime r{}; Observed o; setup(r, true); attach(o, r);
  (void)genesis_runtime_retire_m68k_instruction(&r, 4u, 0x200u);
  GenesisFrameArtifact first = o.frame;
  r.devices.vdp.cram[0] = 0x00u; r.devices.vdp.cram[1] = 0x0Eu;
  (void)genesis_runtime_retire_m68k_instruction(
      &r, (uint32_t)(GENESIS_NTSC_MASTER_TICKS_PER_FRAME / GENESIS_M68K_CYCLE_MASTER_TICKS + 4u), 0x202u);
  check(o.observer.sequence == 2u, "one long retirement across boundaries publishes one latest frame");
  check(std::memcmp(&first, &o.frame, sizeof(first)) != 0, "sequence identifies a fresh artifact, not the stale one");
}
}  // namespace

int main() {
  no_boundary_no_frame();
  exact_onset_one_frame_using_current_vdp_state();
  produces_with_ie0_disabled_and_irq_masked();
  irq_admission_does_not_select_frame_existence();
  unrenderable_publishes_nothing();
  observer_is_non_semantic();
  multiple_boundaries_latest_frame_and_sequence();
  return failures == 0 ? 0 : 1;
}
