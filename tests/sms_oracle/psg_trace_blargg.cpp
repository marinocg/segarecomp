// SEG-009-T007: test-only trace adapter for Blargg's Sms_Apu 0.1.4 (LGPL-2.1+), compiled locally from the copy inside the
// pinned Gearsystem checkout. Sms_Apu keeps its oscillators private; like psg_observe_blargg.cpp this is the only
// translation unit that sees them (access macro scoped to the third-party header). Modes (script lines as in
// tests/tools/sn76489_trace.c, `w <CPU clock> <byte>` ... `end <CPU clock>`; Sms_Apu time is the CPU clock, exact to 1):
//   regs   one line per write: `<clock> t0 t1 t2 v0 v1 v2 v3 noise_rate noise_white shifter` (registers after the write)
//   noise  after each write, every change of the noise shifter, checked once per 16 clocks: `<clock> <shifter bit 0>`
//   edges  `edges <write clock> <tone1 register>`: clocks (1-clock resolution) at which the tone 1 phase flips after the
//          write, tone 1 at full volume
//   pcm    44,100 Hz Blip_Buffer output of the script, one signed sample per line
#define private public
#include "Sms_Apu.h"
#undef private

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
struct Rig {
  Sms_Apu apu;
  Blip_Buffer buffer;
  Rig() {
    buffer.clock_rate(3579545);
    buffer.set_sample_rate(44100);
    apu.output(&buffer);
    apu.reset(false);  // Sega integrated variant
  }
  int noise_rate() const {
    if (apu.noise.period == &apu.squares[2].period) return 3;
    return *apu.noise.period == 0x100 ? 0 : *apu.noise.period == 0x200 ? 1 : 2;
  }
};
struct W { int cycles; int value; };
}  // namespace

int main(int argc, char** argv) {
  std::string mode = argc > 1 ? argv[1] : "regs";
  Rig r;
  if (mode == "edges") {
    const int t = std::atoi(argv[2]);
    const int reg = std::atoi(argv[3]);
    r.apu.write_data(0, 0xB0);  // tone 1 attenuation 0
    r.apu.write_data(t, 0xA0 | (reg & 15));
    r.apu.write_data(t, (reg >> 4) & 0x3F);
    int last = r.apu.squares[1].phase;
    int now = t;
    int found = 0;
    while (found < 6 && now < t + 16 * (reg ? reg : 1) * 8 + 64) {
      ++now;
      r.apu.run_until(now);
      if (r.apu.squares[1].phase != last) { last = r.apu.squares[1].phase; std::printf("%d\n", now - 1); ++found; }
      if (now > 200000) break;
    }
    return 0;
  }
  std::vector<W> writes;
  int end = 0;
  char kind[8];
  long long a;
  unsigned b;
  while (std::scanf("%7s %lld", kind, &a) == 2) {
    if (std::strcmp(kind, "end") == 0) { end = static_cast<int>(a); break; }
    if (std::scanf("%u", &b) != 1) return 2;
    writes.push_back({static_cast<int>(a), static_cast<int>(b)});
  }
  if (mode == "regs") {
    for (const W& w : writes) {
      r.apu.write_data(w.cycles, w.value);
      const Sms_Apu& a2 = r.apu;
      std::printf("%d %d %d %d %d %d %d %d %d %d %u\n", w.cycles, a2.squares[0].period >> 4, a2.squares[1].period >> 4,
                  a2.squares[2].period >> 4, a2.squares[0].volume_reg, a2.squares[1].volume_reg, a2.squares[2].volume_reg,
                  a2.noise.volume_reg, r.noise_rate(), a2.noise.feedback == a2.noise_feedback ? 1 : 0, a2.noise.shifter);
    }
  } else if (mode == "noise") {
    size_t w = 0;
    unsigned last = r.apu.noise.shifter;
    for (int now = 0; now < end; now += 16) {
      while (w < writes.size() && writes[w].cycles <= now) { r.apu.write_data(writes[w].cycles, writes[w].value); last = r.apu.noise.shifter; ++w; }
      r.apu.run_until(now + 16);
      if (r.apu.noise.shifter != last) { last = r.apu.noise.shifter; std::printf("%d %u\n", now + 16, last & 1u); }
    }
  } else {  // pcm
    for (const W& w : writes) r.apu.write_data(w.cycles, w.value);
    r.apu.end_frame(end);
    r.buffer.end_frame(end);
    blip_sample_t buf[16384];
    long n = r.buffer.read_samples(buf, 16384);
    for (long i = 0; i < n; ++i) std::printf("%d\n", static_cast<int>(buf[i]));
  }
  return 0;
}
