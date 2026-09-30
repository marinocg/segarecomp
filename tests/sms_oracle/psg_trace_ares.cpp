// SEG-009-T007: test-only tick-level trace adapter for the ares SN76489 component (ISC), compiled locally from the
// pinned checkout (see tests/sms_psg_differential_test.py). Reads the same write script as tests/tools/sn76489_trace.c
// (`w <master clock> <byte>` ... `end <master clock>`) and prints, before each chip tick j, the writes whose
// floor(clock / 16) <= j, then the state after the tick: `j o0 o1 o2 c0 c1 c2 p0 p1 p2 noise_counter flip lfsr a0 a1 a2 a3
// noise_control`. It never decides pass or fail.
#include <ares/ares.hpp>
#include <component/audio/sn76489/sn76489.hpp>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
struct Probe : ares::SN76489 {
  using SN76489::latch;
  using SN76489::noise;
  using SN76489::tone0;
  using SN76489::tone1;
  using SN76489::tone2;
};
}  // namespace

int main() {
  struct W { unsigned long long cycles; unsigned value; };
  std::vector<W> writes;
  unsigned long long end = 0, a = 0;
  char kind[8];
  unsigned b;
  while (std::scanf("%7s %llu", kind, &a) == 2) {
    if (std::strcmp(kind, "end") == 0) { end = a; break; }
    if (std::scanf("%u", &b) != 1) return 2;
    writes.push_back({a, b});
  }
  Probe p;
  p.power();
  std::size_t w = 0;
  for (unsigned long long j = 0; j < end / 16; ++j) {
    while (w < writes.size() && writes[w].cycles / 16 <= j) p.write(static_cast<unsigned>(writes[w++].value));
    p.clock();
    std::printf("%llu %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u\n", j, (unsigned)p.tone0.output,
                (unsigned)p.tone1.output, (unsigned)p.tone2.output, (unsigned)p.tone0.counter, (unsigned)p.tone1.counter,
                (unsigned)p.tone2.counter, (unsigned)p.tone0.pitch, (unsigned)p.tone1.pitch, (unsigned)p.tone2.pitch,
                (unsigned)p.noise.counter, (unsigned)p.noise.flip, (unsigned)p.noise.lfsr, (unsigned)p.tone0.volume,
                (unsigned)p.tone1.volume, (unsigned)p.tone2.volume, (unsigned)p.noise.volume,
                (unsigned)((p.noise.enable << 2) | p.noise.rate));
  }
  return 0;
}
