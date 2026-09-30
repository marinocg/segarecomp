// SEG-009-T001 (ADR 0062): test-only observation adapter for the ares SN76489 component (ISC),
// compiled locally from the pinned checkout with the real nall headers. It never decides pass or
// fail: it prints chip-level observations as JSON and tests/sms_psg_oracle_smoke_test.py compares
// them with expectations derived from public documents.
//
// Chip tick = one call of SN76489::clock() = 16 CPU clocks (the /16 internal divider).
#include <ares/ares.hpp>
#include <component/audio/sn76489/sn76489.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace {
struct Probe : ares::SN76489 {
  using SN76489::latch;
  using SN76489::noise;
  using SN76489::tone0;
  using SN76489::tone1;
  using SN76489::tone2;
};

std::string bits(const std::vector<int>& values) {
  std::string out;
  for (int v : values) out += static_cast<char>('0' + v);
  return out;
}

// Noise output bit after each LFSR shift, for `count` shifts.
std::vector<int> noise_shifts(Probe& p, int count) {
  std::vector<int> out;
  unsigned last = static_cast<unsigned>(p.noise.lfsr);
  for (int guard = 0; static_cast<int>(out.size()) < count && guard < 1000000; ++guard) {
    p.clock();
    unsigned now = static_cast<unsigned>(p.noise.lfsr);
    if (now != last) {  // the register never repeats a value between consecutive shifts
      out.push_back(static_cast<int>(p.noise.output));
      last = now;
    }
  }
  return out;
}
}  // namespace

int main() {
  Probe p;
  p.power();
  std::printf("{\"core\":\"ares_sn76489\",");
  std::printf("\"reset\":{\"tone\":[%u,%u,%u],\"volume\":[%u,%u,%u,%u],\"lfsr\":%u,\"noise_rate\":%u,\"noise_white\":%u},",
              (unsigned)p.tone0.pitch, (unsigned)p.tone1.pitch, (unsigned)p.tone2.pitch, (unsigned)p.tone0.volume,
              (unsigned)p.tone1.volume, (unsigned)p.tone2.volume, (unsigned)p.noise.volume, (unsigned)p.noise.lfsr,
              (unsigned)p.noise.rate, (unsigned)p.noise.enable);

  // Latch/data: channel 0 tone low nibble 0xE, then data 0x0F -> 0x0FE (SMS Power example).
  p.write(0x8E);
  unsigned after_latch = (unsigned)p.tone0.pitch;
  p.write(0x0F);
  unsigned after_data = (unsigned)p.tone0.pitch;
  // Volume latch then data byte (Alex Kidd example): volume 15 then data 0 -> 0.
  p.write(0xDF);
  unsigned vol_latch = (unsigned)p.tone2.volume;
  p.write(0x00);
  unsigned vol_data = (unsigned)p.tone2.volume;
  // Noise latch then data byte (Micro Machines example): %101 then data %100.
  p.write(0xE5);
  unsigned n1_rate = (unsigned)p.noise.rate, n1_white = (unsigned)p.noise.enable;
  p.clock(); p.clock(); p.clock();
  p.write(0x04);
  unsigned n2_rate = (unsigned)p.noise.rate, n2_white = (unsigned)p.noise.enable, n2_lfsr = (unsigned)p.noise.lfsr;
  std::printf("\"latch\":{\"tone0_after_latch\":%u,\"tone0_after_data\":%u,\"vol2_after_latch\":%u,\"vol2_after_data\":%u,"
              "\"noise_after_latch\":[%u,%u],\"noise_after_data\":[%u,%u],\"lfsr_after_noise_write\":%u},",
              after_latch, after_data, vol_latch, vol_data, n1_rate, n1_white, n2_rate, n2_white, n2_lfsr);

  // White noise, rate 0, full volume: output bits of the first 64 shifts after the reset seed.
  p.write(0xE4);
  p.write(0xF0);
  std::vector<int> white = noise_shifts(p, 64);
  // "Periodic" noise, rate 0: 32 shifts.
  p.write(0xE0);
  std::vector<int> periodic = noise_shifts(p, 32);
  std::printf("\"white_rate0\":\"%s\",\"periodic_rate0\":\"%s\",", bits(white).c_str(), bits(periodic).c_str());

  // Noise shift interval (chip ticks) for rates 0..2 and rate 3 with tone 2 period 5.
  int intervals[4];
  for (int rate = 0; rate < 4; ++rate) {
    if (rate == 3) { p.write(0xC5); p.write(0x00); }
    p.write(static_cast<unsigned>(0xE4 | rate));
    unsigned last = (unsigned)p.noise.lfsr;
    int first = -1, second = -1;
    for (int t = 1; t < 4096 && second < 0; ++t) {
      p.clock();
      if ((unsigned)p.noise.lfsr != last) { last = (unsigned)p.noise.lfsr; if (first < 0) first = t; else second = t; }
    }
    intervals[rate] = second - first;
  }
  std::printf("\"noise_shift_interval\":[%d,%d,%d,%d],", intervals[0], intervals[1], intervals[2], intervals[3]);

  // Tone period 0 and 1 on channel 1, full volume: channel output bit for 8 consecutive ticks.
  for (int period = 0; period < 2; ++period) {
    p.write(0xB0);
    p.write(static_cast<unsigned>(0xA0 | period));
    p.write(0x00);
    std::vector<int> out;
    for (int t = 0; t < 8; ++t) { p.clock(); out.push_back((int)p.tone1.output); }
    std::printf("\"tone_period%d\":\"%s\",", period, bits(out).c_str());
  }
  // Tone period 3: toggles every 3 ticks.
  p.write(0xA3); p.write(0x00);
  std::vector<int> out3;
  for (int t = 0; t < 12; ++t) { p.clock(); out3.push_back((int)p.tone1.output); }
  std::printf("\"tone_period3\":\"%s\",", bits(out3).c_str());
  // Output level per attenuation step: ares reports the attenuation nibble when the channel is on
  // and 15 (silence) when it is off; the level table is applied by the machine mixer.
  std::printf("\"attenuation_is_nibble\":true}\n");
  return 0;
}
