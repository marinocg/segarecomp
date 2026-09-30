// SEG-009-T007: Sega PSG (SN76489-family) device and PCM stream, hermetic unit tests linked against the device library
// only (no SMS, Genesis or CPU target). Expectations are derived here from SMS Power! "SN76489" (Maxim) and the machine
// contract section 10, never from the code under test; the cross-implementation and pinned-reference differentials are
// tests/sms_psg_differential_test.py. The conformance function also runs against mutated device parameters (wrong taps,
// wrong divider, wrong attenuation table): every mutation must be detected.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "segarecomp/device/sega/psg/sn76489.h"

namespace {

int failures = 0;
void check(bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::printf("FAIL: %s\n", message.c_str());
  }
}

// SP-PSG reference implementation of the noise register (16 bits, taps bit 0 and 3, output = bit 0 after the shift).
std::vector<int> lfsr_bits(bool white, int count, unsigned width = 16, unsigned taps = 0x0009) {
  unsigned reg = 1u << (width - 1u);
  std::vector<int> out;
  for (int i = 0; i < count; ++i) {
    unsigned fb = white ? static_cast<unsigned>(__builtin_parity(reg & taps)) : (reg & 1u);
    reg = (reg >> 1) | (fb << (width - 1u));
    out.push_back(static_cast<int>(reg & 1u));
  }
  return out;
}

struct Rig {
  Sn76489 chip;
  explicit Rig(const Sn76489Config& config) { sn76489_init(&chip, &config); }
  void tick(unsigned n = 1) { while (n--) sn76489_tick(&chip, nullptr); }
  void write(unsigned v) { sn76489_write(&chip, static_cast<uint8_t>(v)); }
};

// Noise output bit after every LFSR shift, for `count` shifts (LFSR value changes, or the periodic register repeats:
// count shifts by the flip-flop edge instead, which is observable as `noise_flip` becoming 1).
std::vector<int> noise_bits(Rig& r, int count) {
  std::vector<int> out;
  uint8_t last_flip = r.chip.noise_flip;
  for (int guard = 0; static_cast<int>(out.size()) < count && guard < 1000000; ++guard) {
    r.tick();
    if (r.chip.noise_flip != last_flip) {
      last_flip = r.chip.noise_flip;
      if (r.chip.noise_flip) out.push_back(static_cast<int>(r.chip.lfsr & 1u));
    }
  }
  return out;
}

// Returns the number of violated expectations for the given device parameters; 0 = the contract model.
int conformance(const Sn76489Config& config, bool record) {
  int bad = 0;
  auto expect = [&](bool ok, const char* what) {
    if (!ok) {
      ++bad;
      if (record) check(false, what);
    }
  };
  {
    Rig r(config);
    // reset state: tone 0, attenuation $F, LFSR seed, nothing latched
    bool reset_ok = r.chip.latch == -1 && r.chip.lfsr == 0x8000 && r.chip.noise_control == 0 && r.chip.ticks == 0;
    for (int i = 0; i < 3; ++i) reset_ok = reset_ok && r.chip.tone_period[i] == 0;
    for (int i = 0; i < 4; ++i) reset_ok = reset_ok && r.chip.attenuation[i] == 15;
    expect(reset_ok, "reset state");
    expect(sn76489_level(&r.chip) == 0, "silent after reset");
  }
  {
    Rig r(config);
    expect(sn76489_write(&r.chip, 0x0F) == SN76489_WRITE_DATA_BEFORE_LATCH, "data before latch is reported");
    expect(r.chip.tone_period[0] == 0 && r.chip.latch == -1, "data before latch leaves the state unchanged");
    r.write(0x8E);  // SP-PSG example: channel 0 tone low nibble $E
    expect(r.chip.tone_period[0] == 0x00E, "tone latch writes the low nibble immediately");
    r.write(0x0F);
    expect(r.chip.tone_period[0] == 0x0FE, "tone data byte writes the high six bits (440 Hz example $0FE)");
    r.write(0x8F);  // low nibble again keeps the high bits
    expect(r.chip.tone_period[0] == 0x0FF, "tone latch keeps the high bits");
    r.write(0xDF);  // channel 2 attenuation latch
    r.write(0x00);  // Alex Kidd: a data byte after an attenuation latch is NOT ignored
    expect(r.chip.attenuation[2] == 0, "data byte after an attenuation latch updates it");
    r.write(0xE5);  // noise latch %101: white, rate 1
    expect((r.chip.noise_control & 7) == 5, "noise latch");
    r.tick(3);
    r.write(0x04);  // Micro Machines: data byte %100 updates the noise register
    expect((r.chip.noise_control & 7) == 4 && r.chip.lfsr == 0x8000, "noise data byte updates the register and reloads the LFSR");
    r.write(0xC3);  // tone 2 period low nibble
    r.write(0x3F);
    expect(r.chip.tone_period[2] == 0x3F3, "tone 2 period");
    r.write(0x7F);  // data byte continues the tone-2 latch: high bits $3F again
    expect(r.chip.tone_period[2] == 0x3F3, "repeated data byte is idempotent");
  }
  {
    // tone period 3: flips when the counter reloads: 1 1 1 0 0 0 (counter starts at 0 and reloads on the first tick)
    Rig r(config);
    r.write(0x83);
    r.write(0x00);
    std::string seq;
    for (int i = 0; i < 12; ++i) {
      r.tick();
      seq += static_cast<char>('0' + r.chip.tone_output[0]);
    }
    expect(seq == "111000111000", "tone period 3 square wave");
    // periods 0 and 1 toggle every tick (project decision U10)
    for (unsigned period = 0; period < 2; ++period) {
      Rig q(config);
      q.write(0x80 | period);
      q.write(0x00);
      std::string s;
      for (int i = 0; i < 8; ++i) {
        q.tick();
        s += static_cast<char>('0' + q.chip.tone_output[0]);
      }
      expect(s == "10101010", "tone period 0 and 1 toggle every tick");
    }
    // a period change takes effect when the counter next reloads, it does not reload the counter
    Rig p(config);
    p.write(0x85);
    p.write(0x00);
    p.tick(2);  // counter reloaded to 5 at tick 1, now 4
    p.write(0x82);
    p.tick(2);  // counter 3, 2: no flip yet
    expect(p.chip.tone_output[0] == 1, "a period write does not reload the counter");
    p.tick(2);  // 1, then reload to 2 with a flip
    expect(p.chip.tone_output[0] == 0, "the new period applies at the next reload");
  }
  {
    // noise: shift intervals in chip ticks for each rate (one shift per two counter expiries)
    const int expected_interval[4] = {32, 64, 128, 10};
    for (int rate = 0; rate < 4; ++rate) {
      Rig r(config);
      r.write(0xC5);  // tone 2 period 5 (used by rate 3)
      r.write(0x00);
      r.write(0xE4u | static_cast<unsigned>(rate));
      unsigned last = r.chip.lfsr;
      int first = -1, second = -1;
      for (int t = 1; t < 4096 && second < 0; ++t) {
        r.tick();
        if (r.chip.lfsr != last) {
          last = r.chip.lfsr;
          (first < 0 ? first : second) = t;
        }
      }
      expect(second - first == expected_interval[rate], "noise shift interval");
    }
    // LFSR sequences, white and periodic, every rate shares the register behaviour
    for (int white = 0; white < 2; ++white) {
      for (int rate = 0; rate < 4; ++rate) {
        Rig r(config);
        r.write(0xC2);  // tone 2 period 2 keeps rate 3 fast
        r.write(0x00);
        r.write(0xE0u | (white ? 4u : 0u) | static_cast<unsigned>(rate));
        const std::vector<int> got = noise_bits(r, 80);
        expect(got == lfsr_bits(white != 0, 80), "noise bit sequence");
      }
    }
    // periodic noise repeats every 16 shifts
    Rig r(config);
    r.write(0xE0);
    const std::vector<int> periodic = noise_bits(r, 48);
    bool repeats = true;
    for (size_t i = 0; i < 32; ++i) repeats = repeats && periodic[i] == periodic[i + 16];
    expect(repeats, "periodic noise repeats every 16 shifts");
    // any noise-register write reloads the LFSR: after running, a latch write and a data write both reseed
    Rig q(config);
    q.write(0xE4);
    q.tick(200);
    expect(q.chip.lfsr != 0x8000, "the LFSR has advanced");
    q.write(0xE4);
    expect(q.chip.lfsr == 0x8000, "noise latch write reloads the LFSR");
    q.tick(200);
    q.write(0x05);
    expect(q.chip.lfsr == 0x8000, "noise data write reloads the LFSR");
  }
  {
    // attenuation: each step is ~2 dB, $F silences; the integer table is the SMS Power! table verbatim
    static const uint16_t table[16] = {32767, 26028, 20675, 16422, 13045, 10362, 8231, 6568,
                                       5193,  4125,  3277,  2603,  2067,  1642,  1304, 0};
    Rig r(config);
    r.write(0x81);  // tone 0 period 1: toggles
    r.write(0x00);
    r.tick();  // output 1
    bool levels_ok = r.chip.tone_output[0] == 1;
    for (unsigned a = 0; a < 16; ++a) {
      r.write(0x90 | a);
      levels_ok = levels_ok && sn76489_level(&r.chip) == table[a];
    }
    expect(levels_ok, "attenuation table");
  }
  {
    // timing: the device runs whole ticks; a split advance equals a single advance
    Rig a(config), b(config);
    for (Rig* r : {&a, &b}) {
      r->write(0x82);
      r->write(0x01);
      r->write(0x90);
    }
    sn76489_advance(&a.chip, nullptr, 16u * 1000u + 15u);
    sn76489_advance(&b.chip, nullptr, 7u);
    sn76489_advance(&b.chip, nullptr, 300u);
    sn76489_advance(&b.chip, nullptr, 16u * 1000u + 15u);
    sn76489_advance(&b.chip, nullptr, 5u);  // the past does nothing
    uint8_t sa[SN76489_STATE_BYTES], sb[SN76489_STATE_BYTES];
    sn76489_state_bytes(&a.chip, sa);
    sn76489_state_bytes(&b.chip, sb);
    expect(a.chip.ticks == 1000 && std::memcmp(sa, sb, sizeof sa) == 0, "split advance equals single advance, 16 T per tick");
  }
  return bad;
}

void test_pcm() {
  const Sn76489Config config = sn76489_default_config();
  const Sn76489PcmConfig pc = sn76489_pcm_config_44100_sms();
  // T_k = ceil(k x 39,375,000 / 485,100): 0, 82, 163, 244
  check(sn76489_pcm_sample_start(&pc, 0) == 0 && sn76489_pcm_sample_start(&pc, 1) == 82 &&
            sn76489_pcm_sample_start(&pc, 2) == 163 && sn76489_pcm_sample_start(&pc, 3) == 244,
        "sample start times");
  // one frame holds 735 whole samples: T_735 = 59,660 and T_736 = 59,741 bracket 59,736
  check(sn76489_pcm_sample_start(&pc, 735) <= 59736 && sn76489_pcm_sample_start(&pc, 736) > 59736, "samples per frame");
  {
    std::vector<int16_t> samples;
    Sn76489Pcm pcm;
    sn76489_pcm_init(&pcm, &pc,
                     [](void* ctx, uint64_t first, const int16_t* s, uint32_t n) {
                       auto* v = static_cast<std::vector<int16_t>*>(ctx);
                       check(first == v->size(), "sink receives contiguous samples");
                       v->insert(v->end(), s, s + n);
                     },
                     &samples);
    Sn76489 chip;
    sn76489_init(&chip, &config);
    sn76489_advance(&chip, &pcm, 59736);
    sn76489_pcm_flush(&pcm);
    // silent chip: mean 0 -> -32,768 everywhere; 3,733 whole ticks end at 59,728 and T_735 = 59,660 is the last sample start at or before it
    bool silent = true;
    for (int16_t s : samples) silent = silent && s == -32768;
    check(silent && samples.size() == 735, "silent stream: DC policy is the fixed -32,768 offset");
    // tone 0 with period 1000 is constant over a short run: channel on at level[0]: mean 32,767
    Sn76489 loud;
    sn76489_init(&loud, &config);
    sn76489_write(&loud, 0x80 | 0x08);  // tone 0 low nibble 8
    sn76489_write(&loud, 0x3E);         // period $3E8 = 1000
    sn76489_write(&loud, 0x90);         // attenuation 0
    std::vector<int16_t> v2;
    Sn76489Pcm pcm2;
    sn76489_pcm_init(&pcm2, &pc,
                     [](void* ctx, uint64_t, const int16_t* s, uint32_t n) {
                       auto* v = static_cast<std::vector<int16_t>*>(ctx);
                       v->insert(v->end(), s, s + n);
                     },
                     &v2);
    sn76489_advance(&loud, &pcm2, 16 * 200);
    sn76489_pcm_flush(&pcm2);
    check(v2.size() >= 30 && v2[10] == -16385, "full-level channel: floor(32,767 x 65,535 / 131,068) - 32,768 = -16,385");
  }
  {
    // slice independence: the same write timeline advanced in different slices gives the same samples
    auto run = [&](const std::vector<uint64_t>& cuts) {
      std::vector<int16_t> out;
      Sn76489Pcm pcm;
      sn76489_pcm_init(&pcm, &pc,
                       [](void* ctx, uint64_t, const int16_t* s, uint32_t n) {
                         static_cast<std::vector<int16_t>*>(ctx)->insert(static_cast<std::vector<int16_t>*>(ctx)->end(), s, s + n);
                       },
                       &out);
      Sn76489 chip;
      sn76489_init(&chip, &config);
      struct W { uint64_t t; uint8_t v; };
      const W writes[] = {{10, 0x84}, {27, 0x05}, {50, 0x90}, {900, 0x85}, {2000, 0xE4}, {2001, 0xF0}, {30000, 0x9F}};
      size_t cut = 0;
      for (const W& w : writes) {
        while (cut < cuts.size() && cuts[cut] <= w.t) sn76489_advance(&chip, &pcm, cuts[cut++]);  // arbitrary host slices
        sn76489_advance(&chip, &pcm, w.t);
        sn76489_write(&chip, w.v);
      }
      sn76489_advance(&chip, &pcm, 59736);
      sn76489_pcm_flush(&pcm);
      return out;
    };
    const std::vector<int16_t> a = run({}), b = run({3, 9, 15, 16, 17, 1000, 1999, 2000, 29999}), c = run({1, 2, 3, 4, 5, 6, 7, 8, 9});
    check(a == b && a == c && !a.empty(), "PCM is independent of host slice boundaries");
    bool varies = false;
    for (int16_t s : a) varies = varies || s != a[0];
    check(varies, "the timeline produces a non-constant stream");
  }
  {
    // ring: whole samples, oldest dropped and counted on overflow
    int16_t storage[4];
    Sn76489Ring ring;
    sn76489_ring_init(&ring, storage, 4);
    const int16_t in[6] = {1, 2, 3, 4, 5, 6};
    sn76489_ring_sink(&ring, 0, in, 6);
    check(ring.dropped == 2 && sn76489_ring_available(&ring) == 4, "ring overflow drops the oldest samples");
    int16_t out[8];
    uint64_t first = 99;
    const uint32_t n = sn76489_ring_read(&ring, out, 8, &first);
    check(n == 4 && first == 2 && out[0] == 3 && out[3] == 6, "ring read returns the newest samples in order");
  }
}

}  // namespace

int main() {
  const Sn76489Config contract = sn76489_default_config();
  check(conformance(contract, true) == 0, "contract model conforms");
  test_pcm();

  // mutation controls: each wrong parameter must be detected by the conformance checks
  Sn76489Config wrong_taps = contract;
  wrong_taps.noise_taps = 0x0003;  // the TI chip's taps
  check(conformance(wrong_taps, false) > 0, "mutation: wrong LFSR taps detected");
  Sn76489Config wrong_width = contract;
  wrong_width.lfsr_bits = 15;
  check(conformance(wrong_width, false) > 0, "mutation: wrong LFSR width detected");
  Sn76489Config wrong_divider = contract;
  wrong_divider.divider = 8;
  {
    // the divider only changes timing: detect it through the tick count of a fixed-time advance
    Sn76489 a, b;
    sn76489_init(&a, &contract);
    sn76489_init(&b, &wrong_divider);
    sn76489_advance(&a, nullptr, 1600);
    sn76489_advance(&b, nullptr, 1600);
    check(a.ticks == 100 && b.ticks != a.ticks, "mutation: wrong divider detected");
  }
  static uint16_t bad_table[16];
  std::memcpy(bad_table, sn76489_default_levels, sizeof bad_table);
  bad_table[7] = 6569;
  Sn76489Config wrong_table = contract;
  wrong_table.levels = bad_table;
  check(conformance(wrong_table, false) > 0, "mutation: wrong attenuation table detected");
  bad_table[7] = 6568;
  bad_table[15] = 1;  // $F must silence
  check(conformance(wrong_table, false) > 0, "mutation: non-silent $F detected");

  Sn76489 scratch;
  Sn76489Config invalid = contract;
  invalid.lfsr_bits = 17;
  check(sn76489_init(&scratch, &invalid) == 0, "an unusable width is rejected");
  invalid = contract;
  invalid.divider = 0;
  check(sn76489_init(&scratch, &invalid) == 0, "a zero divider is rejected");

  if (failures == 0) std::printf("sn76489 tests passed\n");
  return failures == 0 ? 0 : 1;
}
