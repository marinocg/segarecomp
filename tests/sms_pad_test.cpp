// SEG-009-T006: Master System controller ports, I/O control and TH-latched H counter (machine contract sections 9.7, 11).
//
// Expectations come from an independent reference written here from the pinout tables of the contract (MacDonald SMS
// hardware notes section 4): every pin of the four-pin-per-port I/O chip is named and resolved one by one, never by the
// bit tricks of the device under test. The exhaustive sweep covers every pad state and every I/O-control byte.
// A mutation control proves the sweep discriminates: an inverted-polarity reference must be detected. Pause NMI edges
// are checked generated-native (tests/sms_pad_native_test.py).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "sms_pad.h"
#include "sms_vdp.h"

// The generated Z80 image is not part of this test: the machine is only used as the port/digest host.
namespace {

extern "C" Z80Outcome z80_run(Z80Runtime* rt, uint64_t) { return rt->outcome; }

int failures = 0;
void check(bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::printf("FAIL: %s\n", message.c_str());
  }
}

struct Pins {  // the eight pins of ports A and B as seen by the I/O chip
  bool a_up, a_down, a_left, a_right, a_tl, a_tr, a_th;
  bool b_up, b_down, b_left, b_right, b_tl, b_tr, b_th;
};

// Pad side: released = high, pressed = low (polarity is inverted in the mutation).
Pins pad_pins(const SmsInputState& in, bool inverted) {
  auto line = [&](uint8_t mask, int bit) { return ((mask >> bit) & 1) != 0 ? inverted : !inverted; };
  Pins p{};
  p.a_up = line(in.p1, 0), p.a_down = line(in.p1, 1), p.a_left = line(in.p1, 2), p.a_right = line(in.p1, 3);
  p.a_tl = line(in.p1, 4), p.a_tr = line(in.p1, 5), p.a_th = true;
  p.b_up = line(in.p2, 0), p.b_down = line(in.p2, 1), p.b_left = line(in.p2, 2), p.b_right = line(in.p2, 3);
  p.b_tl = line(in.p2, 4), p.b_tr = line(in.p2, 5), p.b_th = true;
  return p;
}

// Output side (port $3F): bit3..0 = B.TH, B.TR, A.TH, A.TR direction (1 = input), bit7..4 = their output levels.
void apply_outputs(Pins& p, uint8_t ctrl) {
  if (((ctrl >> 0) & 1) == 0) p.a_tr = ((ctrl >> 4) & 1) != 0;
  if (((ctrl >> 1) & 1) == 0) p.a_th = ((ctrl >> 5) & 1) != 0;
  if (((ctrl >> 2) & 1) == 0) p.b_tr = ((ctrl >> 6) & 1) != 0;
  if (((ctrl >> 3) & 1) == 0) p.b_th = ((ctrl >> 7) & 1) != 0;
}

uint8_t pack(const bool (&bits)[8]) {
  uint8_t v = 0;
  for (int i = 0; i < 8; ++i)
    if (bits[i]) v = static_cast<uint8_t>(v | (1u << i));
  return v;
}

uint8_t ref_dc(const SmsInputState& in, uint8_t ctrl, bool inverted = false) {
  Pins p = pad_pins(in, inverted);
  apply_outputs(p, ctrl);
  const bool bits[8] = {p.a_up, p.a_down, p.a_left, p.a_right, p.a_tl, p.a_tr, p.b_up, p.b_down};
  return pack(bits);
}
uint8_t ref_dd(const SmsInputState& in, uint8_t ctrl, bool inverted = false) {
  Pins p = pad_pins(in, inverted);
  apply_outputs(p, ctrl);
  const bool bits[8] = {p.b_left, p.b_right, p.b_tl, p.b_tr, true /* reset: no button on the SMS 2 */, true /* CONT */,
                        p.a_th, p.b_th};
  return pack(bits);
}

void sweep() {
  unsigned checked = 0, mutant_detected_dc = 0, mutant_detected_dd = 0;
  for (unsigned a = 0; a < 64; ++a) {
    for (unsigned b = 0; b < 64; ++b) {
      for (unsigned ctrl = 0; ctrl < 256; ++ctrl) {
        SmsInputState in{};
        in.p1 = static_cast<uint8_t>(a);
        in.p2 = static_cast<uint8_t>(b);
        const uint8_t dc = sms_pad_port_dc(&in, static_cast<uint8_t>(ctrl));
        const uint8_t dd = sms_pad_port_dd(&in, static_cast<uint8_t>(ctrl));
        ++checked;
        if (dc != ref_dc(in, static_cast<uint8_t>(ctrl)) || dd != ref_dd(in, static_cast<uint8_t>(ctrl))) {
          check(false, "port value differs from reference p1=" + std::to_string(a) + " p2=" + std::to_string(b) +
                           " ctrl=" + std::to_string(ctrl));
          return;
        }
        if (dc != ref_dc(in, static_cast<uint8_t>(ctrl), true)) ++mutant_detected_dc;
        if (dd != ref_dd(in, static_cast<uint8_t>(ctrl), true)) ++mutant_detected_dd;
      }
    }
  }
  check(checked == 64u * 64u * 256u, "sweep size");
  check(mutant_detected_dc > 0 && mutant_detected_dd > 0, "inverted-polarity mutant is not detected");
}

void anchors() {  // values stated by the contract and the T001 oracle smoke ($F5 reads $C0, $55 reads $00 in $DD & $C0)
  SmsInputState idle{};
  check(sms_pad_port_dc(&idle, 0xFF) == 0xFF, "idle DC is $FF");
  check(sms_pad_port_dd(&idle, 0xFF) == 0xFF, "idle DD is $FF (reset and CONT high, TH inputs high)");
  check((sms_pad_port_dd(&idle, 0xF5) & 0xC0) == 0xC0, "$F5: TH outputs high read back high");
  check((sms_pad_port_dd(&idle, 0x55) & 0xC0) == 0x00, "$55: TH outputs low read back low");
  SmsInputState up{};
  up.p1 = SMS_PAD_UP;
  check(sms_pad_port_dc(&up, 0xFF) == 0xFE, "P1 up clears DC bit 0");
  SmsInputState b2{};
  b2.p2 = SMS_PAD_BUTTON2 | SMS_PAD_DOWN;
  check(sms_pad_port_dc(&b2, 0xFF) == 0x7F, "P2 down clears DC bit 7");
  check(sms_pad_port_dd(&b2, 0xFF) == 0xF7, "P2 button 2 clears DD bit 3");
  check(sms_pad_port_dd(&b2, 0xFB) == 0xFF, "B.TR output high level reads 1 (DD bit 3)");
  check(sms_pad_port_dd(&b2, 0xBB) == 0xF7, "B.TR output low level reads 0 regardless of the pad (DD bit 3)");
}

// ---- H counter -------------------------------------------------------------------------------------------------

void hcounter_table() {
  // Sequence of the 171 counts: 0x00-0x93 then 0xE9-0xFF; two pixels per count, 1.5 pixels per T.
  int index_of[256];
  for (int& v : index_of) v = -1;
  for (int i = 0; i < 0x94; ++i) index_of[i] = i;
  for (int i = 0; i < 0x17; ++i) index_of[0xE9 + i] = 0x94 + i;
  int prev = -1;
  int wraps = 0, steps_total = 0;
  for (uint32_t t = 0; t < 228; ++t) {
    const int idx = index_of[sms_vdp_hcounter_value(t)];
    check(idx >= 0, "H counter value outside the documented sequence at T=" + std::to_string(t));
    if (prev >= 0) {
      const int step = (idx - prev + 171) % 171;
      check(step <= 1, "H counter advances by more than one count per T at T=" + std::to_string(t));
      steps_total += step;
      if (idx < prev) ++wraps;
    }
    prev = idx;
  }
  check(wraps == 1, "exactly one sequence wrap per line");
  // 228 T = 342 pixels = 171 counts: one line is one full revolution of the sequence.
  check(steps_total >= 169 && steps_total <= 171, "one line covers the whole 171-count sequence");
}

void latch_behaviour() {
  SmsMachine machine;
  static const uint8_t rom[0x8000] = {0};
  SmsVdp vdp;
  SmsPad pad;
  check(sms_machine_init(&machine, rom, sizeof rom, SMS_MAPPER_ROM_ONLY) == SMS_OK, "machine init");
  std::memset(&vdp, 0, sizeof vdp);
  sms_vdp_install(&machine, &vdp);
  sms_pad_attach(&machine, &pad);
  check(pad.vdp == &vdp, "attach finds the T004 VDP");
  sms_machine_reset(&machine);
  check(pad.io_control == 0xFF && pad.h_latches == 0, "reset state: $FF, no latch");

  // Reference of the trigger: pin level (input reads 1, output reads its level) rising 0 -> 1 on either TH.
  auto pin = [](uint8_t c, int port) { return port == 0 ? (((c >> 1) & 1) ? 1 : (c >> 5) & 1) : (((c >> 3) & 1) ? 1 : (c >> 7) & 1); };
  uint8_t cur = 0xFF;
  unsigned expected = 0;
  uint32_t lcg = 12345;
  for (int i = 0; i < 4000; ++i) {
    lcg = lcg * 1664525u + 1013904223u;
    const uint8_t next = static_cast<uint8_t>(lcg >> 16);
    const uint64_t cycles = 228ull * (lcg % 500u) + (lcg >> 8) % 228u;
    for (int port = 0; port < 2; ++port)
      if (pin(cur, port) == 0 && pin(next, port) == 1) ++expected;
    sms_pad_write(&pad, SMS_PORT_IO_CONTROL, next, cycles);
    cur = next;
    check(pad.h_latches == expected, "latch count after write " + std::to_string(i));
    if (pad.h_latches != expected) return;
  }
  check(expected > 100, "random sequence exercises the trigger");

  // Directed: output low then high latches the value at the write's T offset; the read returns it frozen.
  sms_machine_reset(&machine);
  sms_pad_write(&pad, SMS_PORT_IO_CONTROL, 0xD5, 1000);  // A.TH output low: falling edge, no latch
  check(pad.h_latches == 0, "falling edge does not latch");
  check(sms_vdp_read(&vdp, SMS_PORT_H_COUNTER, 1001) == 0xFF && machine.mem.error == SMS_ERROR_HCOUNTER_UNRESOLVED,
        "read before any latch stops typed");
  sms_machine_reset(&machine);
  const uint64_t when = 228ull * 7 + 100;
  sms_pad_write(&pad, SMS_PORT_IO_CONTROL, 0xD5, when - 50);
  sms_pad_write(&pad, SMS_PORT_IO_CONTROL, 0xF5, when);
  check(pad.h_latches == 1, "rising edge latches");
  const uint8_t latched = sms_vdp_read(&vdp, SMS_PORT_H_COUNTER, when + 40);
  check(latched == sms_vdp_hcounter_value(100), "latched value is the counter at the write's T offset");
  check(sms_vdp_read(&vdp, SMS_PORT_H_COUNTER, when + 1000) == latched, "latch is frozen until the next edge");
  check(machine.mem.error == SMS_OK, "no error after a latched read");
  sms_pad_write(&pad, SMS_PORT_IO_CONTROL, 0xF5, when + 2000);  // level unchanged: no new latch
  check(pad.h_latches == 1, "a repeated high write does not latch");
  sms_pad_write(&pad, SMS_PORT_IO_CONTROL, 0x7D, when + 2001);  // A.TH output high, B.TH input (level bit ignored)
  check(pad.h_latches == 1, "an input TH pin never produces an edge");
  sms_pad_write(&pad, SMS_PORT_IO_CONTROL, 0x5D, when + 2002);  // A.TH output low: falling
  sms_pad_write(&pad, SMS_PORT_IO_CONTROL, 0xFF, when + 228 * 3 + 5);  // A.TH back to input: pin 0 -> 1 latches
  check(pad.h_latches == 2 && sms_vdp_read(&vdp, SMS_PORT_H_COUNTER, when + 5000) == sms_vdp_hcounter_value(105),
        "switching a low output back to input raises the pin and latches");
  uint8_t digest_a[32], digest_b[32];
  sms_machine_digest(&machine, digest_a);
  sms_machine_digest(&machine, digest_b);
  check(std::memcmp(digest_a, digest_b, 32) == 0, "digest is stable");
}

}  // namespace

int main() {
  sweep();
  anchors();
  hcounter_table();
  latch_behaviour();
  if (failures == 0) std::printf("sms_pad_tests: ok\n");
  return failures == 0 ? 0 : 1;
}
