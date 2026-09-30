// SEG-009-T001 (ADR 0062): test-only observation adapter for Blargg's Sms_Apu 0.1.4 (LGPL-2.1+),
// compiled from the copy inside the pinned Gearsystem checkout. It prints chip-level observations
// as JSON; tests/sms_psg_oracle_smoke_test.py owns every expectation and citation.
//
// Sms_Apu keeps its oscillators private and exposes no observation API. This adapter is the only
// translation unit that sees them, through an access macro scoped to the third-party header; the
// oracle is observed, never modified. Time unit = CPU clock; one chip tick = 16 clocks.
#define private public
#include "Sms_Apu.h"
#undef private

#include <cstdio>
#include <string>
#include <vector>

namespace {
std::string bits(const std::vector<int>& values) {
  std::string out;
  for (int v : values) out += static_cast<char>('0' + v);
  return out;
}

struct Rig {
  Sms_Apu apu;
  Blip_Buffer buffer;
  blip_time_t now = 0;
  Rig() {
    buffer.clock_rate(3579545);
    buffer.set_sample_rate(44100);
    apu.output(&buffer);
    apu.reset(false);  // Sega integrated variant (16-bit LFSR, taps 0 and 3)
  }
  void write(int data) { apu.write_data(now, data); }
  void tick() {
    now += 16;
    apu.run_until(now);
    if (now > 100000) {  // keep the Blip_Buffer window bounded
      apu.end_frame(now);
      buffer.end_frame(now);
      buffer.remove_samples(buffer.samples_avail());
      now = 0;
    }
  }
  // Noise output as the channel-on bit: Sms_Apu drives amplitude 0 when shifter bit 0 is 1.
  // Noise rate select: the file-static period table holds 0x100/0x200/0x400; rate 3 aliases tone 2.
  int noise_rate() const {
    if (apu.noise.period == &apu.squares[2].period) return 3;
    return *apu.noise.period == 0x100 ? 0 : *apu.noise.period == 0x200 ? 1 : *apu.noise.period == 0x400 ? 2 : -1;
  }
  int noise_on() const { return (apu.noise.shifter & 1U) ? 0 : 1; }
};

std::vector<int> noise_shifts(Rig& r, int count) {
  std::vector<int> out;
  unsigned last = r.apu.noise.shifter;
  for (int guard = 0; static_cast<int>(out.size()) < count && guard < 1000000; ++guard) {
    r.tick();
    if (r.apu.noise.shifter != last) { out.push_back(r.noise_on()); last = r.apu.noise.shifter; }
  }
  return out;
}
}  // namespace

int main() {
  Rig r;
  const Sms_Apu& a = r.apu;
  std::printf("{\"core\":\"blargg_sms_apu\",");
  std::printf("\"reset\":{\"tone\":[%d,%d,%d],\"volume_reg\":[%d,%d,%d,%d],\"lfsr\":%u,\"latch\":%d},",
              a.squares[0].period >> 4, a.squares[1].period >> 4, a.squares[2].period >> 4, a.squares[0].volume_reg,
              a.squares[1].volume_reg, a.squares[2].volume_reg, a.noise.volume_reg, a.noise.shifter, a.latch);

  r.write(0x8E);
  int after_latch = a.squares[0].period >> 4;
  r.write(0x0F);
  int after_data = a.squares[0].period >> 4;
  r.write(0xDF);
  int vol_latch = a.squares[2].volume_reg;
  r.write(0x00);
  int vol_data = a.squares[2].volume_reg;
  r.write(0xE5);
  int n1_rate = r.noise_rate();
  unsigned n1_feedback = a.noise.feedback;
  r.tick(); r.tick(); r.tick();
  r.write(0x04);
  int n2_rate = r.noise_rate();
  unsigned n2_feedback = a.noise.feedback;
  std::printf("\"latch\":{\"tone0_after_latch\":%d,\"tone0_after_data\":%d,\"vol2_after_latch\":%d,\"vol2_after_data\":%d,"
              "\"noise_after_latch\":[%d,%d],\"noise_after_data\":[%d,%d],\"lfsr_after_noise_write\":%u},",
              after_latch, after_data, vol_latch, vol_data, n1_rate, n1_feedback == a.noise_feedback ? 1 : 0, n2_rate,
              n2_feedback == a.noise_feedback ? 1 : 0, a.noise.shifter);

  r.write(0xE4);
  r.write(0xF0);
  std::vector<int> white = noise_shifts(r, 64);
  r.write(0xE0);
  std::vector<int> periodic = noise_shifts(r, 32);
  std::printf("\"white_rate0\":\"%s\",\"periodic_rate0\":\"%s\",", bits(white).c_str(), bits(periodic).c_str());

  int intervals[4];
  for (int rate = 0; rate < 4; ++rate) {
    if (rate == 3) { r.write(0xC5); r.write(0x00); }
    r.write(0xE4 | rate);
    unsigned last = a.noise.shifter;
    int first = -1, second = -1;
    for (int t = 1; t < 4096 && second < 0; ++t) {
      r.tick();
      if (a.noise.shifter != last) { last = a.noise.shifter; if (first < 0) first = t; else second = t; }
    }
    intervals[rate] = second - first;
  }
  std::printf("\"noise_shift_interval\":[%d,%d,%d,%d],", intervals[0], intervals[1], intervals[2], intervals[3]);

  // Tone channel 1 at full volume: phase bit per tick for periods 0, 1 and 3.
  const int periods[3] = {0, 1, 3};
  for (int i = 0; i < 3; ++i) {
    r.write(0xB0);
    r.write(0xA0 | periods[i]);
    r.write(0x00);
    std::vector<int> out;
    for (int t = 0; t < (periods[i] == 3 ? 12 : 8); ++t) { r.tick(); out.push_back(a.squares[1].phase); }
    std::printf("\"tone_period%d\":\"%s\",", periods[i], bits(out).c_str());
  }
  // Attenuation: the amplitude Sms_Apu assigns to each register value (relative scale).
  std::printf("\"attenuation_levels\":[");
  for (int v = 0; v < 16; ++v) {
    r.write(0x90 | v);
    std::printf("%s%d", v ? "," : "", a.squares[0].volume);
  }
  std::printf("]}\n");
  return 0;
}
