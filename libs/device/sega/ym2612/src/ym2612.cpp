// SEG-032-T007 (ADR 0074): the Genesis YM2612 as a host-clocked C ABI device over the vendored ymfm core.
#include "segarecomp/device/sega/ym2612/ym2612.h"

#include <cstdint>
#include <vector>

#include "ymfm_opn.h"

namespace {

constexpr int64_t kNoEvent = -1;

}  // namespace

struct Ym2612 final : public ymfm::ymfm_interface {
  Ym2612() : chip(*this) { chip.reset(); }

  // ---- ymfm host interface: the host owns the clock (all values are in YM2612 input clocks) ----
  void ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) override {
    timer_expiry[tnum & 1u] = duration_in_clocks < 0 ? kNoEvent : static_cast<int64_t>(now) + duration_in_clocks;
  }
  void ymfm_set_busy_end(uint32_t clocks) override { busy_end = now + clocks; }
  bool ymfm_is_busy() override { return now < busy_end; }
  // The YM2612 IRQ pin is not connected on the Genesis (contract section 8): ymfm_update_irq keeps its default no-op.

  void clock_to(uint64_t target_clock) {
    for (;;) {
      // The earliest event: ties go timer A, then timer B, then the sample that starts at the same clock.
      uint64_t best = next_sample_clock;
      int which = -1;
      for (unsigned t = 0; t < 2u; ++t) {
        if (timer_expiry[t] == kNoEvent) continue;
        const uint64_t when = static_cast<uint64_t>(timer_expiry[t]);
        if (when < best || (when == best && which < 0)) {
          best = when;
          which = static_cast<int>(t);
        }
      }
      if (best > target_clock) break;
      now = best;
      if (which >= 0) {
        timer_expiry[which] = kNoEvent;
        m_engine->engine_timer_expired(static_cast<uint32_t>(which));
        continue;
      }
      ymfm::ym2612::output_data out;
      chip.generate(&out, 1);
      if (sink != nullptr) sink(sink_context, next_sample_clock * YM2612_MASTER_TICKS_PER_CLOCK, out.data[0], out.data[1]);
      ++samples;
      next_sample_clock += YM2612_CLOCKS_PER_SAMPLE;
    }
    if (now < target_clock) now = target_clock;
  }

  ymfm::ym2612 chip;
  uint64_t now = 0;                // current device clock, in YM2612 input clocks
  uint64_t next_sample_clock = 0;  // start of the next native sample
  int64_t timer_expiry[2] = {kNoEvent, kNoEvent};
  uint64_t busy_end = 0;
  uint64_t samples = 0;
  Ym2612SampleSink sink = nullptr;
  void *sink_context = nullptr;
};

extern "C" {

// Plain `new`: with the shim (or any conforming runtime) an allocation failure aborts; `std::nothrow` would drag in the C++ runtime.
Ym2612 *ym2612_create(void) { return new Ym2612(); }

void ym2612_destroy(Ym2612 *chip) { delete chip; }

void ym2612_set_sink(Ym2612 *chip, Ym2612SampleSink sink, void *context) {
  chip->sink = sink;
  chip->sink_context = context;
}

void ym2612_advance(Ym2612 *chip, uint64_t master_ticks) { chip->clock_to(master_ticks / YM2612_MASTER_TICKS_PER_CLOCK); }

void ym2612_reset(Ym2612 *chip, uint64_t master_ticks) {
  chip->clock_to(master_ticks / YM2612_MASTER_TICKS_PER_CLOCK);
  chip->timer_expiry[0] = chip->timer_expiry[1] = kNoEvent;
  chip->busy_end = 0;
  chip->chip.reset();
}

uint8_t ym2612_read(Ym2612 *chip, uint32_t port, uint64_t master_ticks) {
  (void)port;  // every port returns the status byte
  chip->clock_to(master_ticks / YM2612_MASTER_TICKS_PER_CLOCK);
  return chip->chip.read_status();
}

void ym2612_write(Ym2612 *chip, uint32_t port, uint8_t value, uint64_t master_ticks) {
  chip->clock_to(master_ticks / YM2612_MASTER_TICKS_PER_CLOCK);
  chip->chip.write(port & 3u, value);
}

uint64_t ym2612_samples_generated(const Ym2612 *chip) { return chip->samples; }

uint64_t ym2612_state_digest(const Ym2612 *chip) {
  std::vector<uint8_t> bytes;
  ymfm::ymfm_saved_state state(bytes, true);
  const_cast<Ym2612 *>(chip)->chip.save_restore(state);
  uint64_t hash = UINT64_C(0xcbf29ce484222325);
  const auto mix = [&hash](uint8_t byte) { hash = (hash ^ byte) * UINT64_C(0x100000001b3); };
  for (const uint8_t byte : bytes) mix(byte);
  const uint64_t host[] = {chip->now, chip->next_sample_clock, static_cast<uint64_t>(chip->timer_expiry[0]),
                           static_cast<uint64_t>(chip->timer_expiry[1]), chip->busy_end, chip->samples};
  for (const uint64_t value : host)
    for (unsigned i = 0; i < 8u; ++i) mix(static_cast<uint8_t>(value >> (8u * i)));
  return hash;
}

}  // extern "C"
