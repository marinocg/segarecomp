// SEG-009-T004: Master System VDP state, port protocol, status register, frame/line interrupts, mode policy and trace.
//
// Direct API test of platforms/master-system/runtime/sms_vdp.c (no Z80, no generated image). Every expectation is written
// from the machine contract section 9 and the public MacDonald / SMS Power! documents it cites, never read back from the
// code under test. The generated-native and reference-emulator comparisons live in tests/sms_vdp_test.py and
// tests/sms_vdp_oracle_test.py; the independent Python model in tests/sms_vdp_model.py.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sms_machine.h"
#include "sms_vdp.h"

namespace {

// The machine library references the generated program's z80_run; a VDP-only test never runs it (declared inside the
// anonymous namespace like tests/sms_runtime_test.cpp: the ABI header has no C++ linkage guard).
extern "C" Z80Outcome z80_run(Z80Runtime* rt, uint64_t) { return rt->outcome; }

int failures = 0;
void check(bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::printf("FAIL: %s\n", message.c_str());
  }
}

struct Fixture {
  SmsMemory mem;
  SmsVdp vdp;
  std::vector<SmsVdpTraceEntry> trace = std::vector<SmsVdpTraceEntry>(4096);
  uint8_t rom[0x8000];
  Fixture() {
    std::memset(rom, 0, sizeof rom);
    std::memset(&vdp, 0, sizeof vdp);
    sms_memory_init(&mem, rom, sizeof rom, SMS_MAPPER_ROM_ONLY);
    sms_memory_reset(&mem);
    vdp.error_sink = &mem;
    sms_vdp_set_trace(&vdp, trace.data(), static_cast<uint32_t>(trace.size()));
    sms_vdp_reset(&vdp);
  }
  void ctl(uint8_t v, uint64_t t = 0) { sms_vdp_write(&vdp, SMS_PORT_VDP_CONTROL, v, t); }
  void data(uint8_t v, uint64_t t = 0) { sms_vdp_write(&vdp, SMS_PORT_VDP_DATA, v, t); }
  uint8_t rd(uint64_t t = 0) { return sms_vdp_read(&vdp, SMS_PORT_VDP_DATA, t); }
  uint8_t status(uint64_t t = 0) { return sms_vdp_read(&vdp, SMS_PORT_VDP_STATUS, t); }
  uint8_t vcount(uint64_t t = 0) { return sms_vdp_read(&vdp, SMS_PORT_V_COUNTER, t); }
  void setaddr(unsigned code, unsigned address) {
    ctl(static_cast<uint8_t>(address & 0xFF));
    ctl(static_cast<uint8_t>((code << 6) | ((address >> 8) & 0x3F)));
  }
  void reg(unsigned r, uint8_t v) {
    ctl(v);
    ctl(static_cast<uint8_t>(0x80 | r));
  }
  // One scanline event at T = (frame x 262 + line) x 228.
  void line(uint64_t frame, uint32_t l) { sms_vdp_scanline(&vdp, frame, l, (frame * 262u + l) * 228u); }
};

void test_reset_state() {
  Fixture f;
  static const uint8_t expected[11] = {0x36, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFB, 0x00, 0x00, 0x00, 0xFF};
  check(std::memcmp(f.vdp.reg, expected, 11) == 0, "reset registers follow contract section 9.1");
  check(f.vdp.address == 0 && f.vdp.code == 0 && f.vdp.latch_set == 0 && f.vdp.read_buffer == 0, "reset address/code/latch/buffer");
  check(f.vdp.line_counter == 0xFF && f.vdp.frame_pending == 0 && f.vdp.line_pending == 0, "reset line counter and flags");
  bool clear = true;
  for (uint8_t b : f.vdp.vram) clear = clear && b == 0;
  for (uint8_t b : f.vdp.cram) clear = clear && b == 0;
  check(clear, "reset VRAM and CRAM are zero");
  check(sms_vdp_mode(&f.vdp) == SMS_VDP_MODE4_192, "reset registers select Mode 4, 192 lines");
  check(sms_vdp_irq_sources(&f.vdp) == 0, "no interrupt source asserted at reset");
  // reset clears a dirty device
  f.data(0x12);
  f.reg(10, 3);
  f.vdp.frame_pending = 1;
  sms_vdp_reset(&f.vdp);
  check(f.vdp.reg[10] == 0xFF && f.vdp.frame_pending == 0 && f.vdp.vram[0] == 0 && f.vdp.trace_count == 0, "reset is complete");
}

void test_control_and_data_ports() {
  Fixture f;
  // first byte updates the low address byte immediately [cap:vdp.first_byte_low_address]
  f.setaddr(1, 0x2345);
  f.ctl(0x99);
  check(f.vdp.address == 0x2399 && f.vdp.latch_set == 1, "first control byte updates the address low byte at once");
  (void)f.status();
  check(f.vdp.latch_set == 0, "a status read clears the latch");
  f.ctl(0x10);
  f.data(0x00);
  check(f.vdp.latch_set == 0, "a data write clears the latch");
  f.ctl(0x10);
  (void)f.rd();
  check(f.vdp.latch_set == 0, "a data read clears the latch");
  // code 1: VRAM write with auto-increment; write loads the buffer
  f.setaddr(1, 0x0100);
  f.data(0xA1);
  f.data(0xA2);
  check(f.vdp.vram[0x100] == 0xA1 && f.vdp.vram[0x101] == 0xA2 && f.vdp.address == 0x0102 && f.vdp.read_buffer == 0xA2,
        "VRAM writes store, increment and load the read buffer");
  // code 0: the read set-up pre-fetches and increments
  f.setaddr(0, 0x0100);
  check(f.vdp.read_buffer == 0xA1 && f.vdp.address == 0x0101, "a code-0 command pre-fetches VRAM[address] and increments");
  check(f.rd() == 0xA1 && f.rd() == 0xA2, "buffered reads return the buffer, then refill from the incremented address");
  check(f.vdp.address == 0x0103, "each read increments");
  // a write between a set-up and the read returns the written byte's buffer semantics
  f.setaddr(0, 0x0100);
  f.data(0x5C);  // code is still 0: data writes go to VRAM; the byte lands at the pre-fetch-incremented address
  check(f.vdp.vram[0x101] == 0x5C, "data writes go to VRAM under code 0");
  check(f.rd() == 0x5C, "a write loads the buffer, the next read returns it");
  // address wrap
  f.setaddr(1, 0x3FFF);
  f.data(0x77);
  check(f.vdp.address == 0 && f.vdp.vram[0x3FFF] == 0x77, "the address wraps past $3FFF");
  f.setaddr(0, 0x3FFF);
  check(f.vdp.address == 0 && f.vdp.read_buffer == 0x77, "a pre-fetch at $3FFF wraps the address");
  // the second byte replaces the high bits and code only
  f.ctl(0xFF);
  f.ctl(0x40 | 0x15);
  check(f.vdp.address == 0x15FF && f.vdp.code == 1, "the second byte sets the code and address bits 13-8");
}

void test_registers_and_cram() {
  Fixture f;
  for (unsigned r = 0; r <= 10; ++r) f.reg(r, static_cast<uint8_t>(0x10 + r));
  for (unsigned r = 0; r <= 10; ++r) check(f.vdp.reg[r] == 0x10 + r, "register " + std::to_string(r) + " stores its value");
  f.reg(11, 0xEE);
  f.reg(15, 0xEE);
  check(f.vdp.reg[0] == 0x10 && f.vdp.reg[10] == 0x1A, "registers 11-15 have no effect");
  check(f.vdp.code == 2, "a register write leaves code 2");
  f.data(0x33);  // code 2: data writes go to VRAM
  check(f.vdp.vram[f.vdp.address - 1] == 0x33, "data writes under code 2 still go to VRAM");
  // a register write also sets the address from the second byte (bits 5-0) and the first byte (low)
  f.ctl(0x42);
  f.ctl(0x80 | 0x20 | 3);
  check(f.vdp.address == 0x2342 && f.vdp.reg[3] == 0x42, "a register write updates the address (second byte bits 5-0, first byte) and the register");
  // CRAM: code 3, 32 bytes, --BBGGRR, $20-$3F alias $00-$1F
  Fixture g;
  g.setaddr(3, 0x0001);
  g.data(0xFF);
  check(g.vdp.cram[1] == 0x3F, "CRAM stores the low 6 bits");
  g.setaddr(3, 0x0021);
  g.data(0x15);
  check(g.vdp.cram[1] == 0x15 && g.vdp.vram[0x21] == 0, "CRAM addresses $20-$3F alias $00-$1F");
  g.setaddr(3, 0x001F);
  g.data(0x01);
  g.data(0x02);
  check(g.vdp.cram[0x1F] == 0x01 && g.vdp.cram[0] == 0x02 && g.vdp.address == 0x21, "CRAM writes advance the 14-bit address");
  check(g.vdp.read_buffer == 0x02, "a CRAM write loads the read buffer");
}

void test_status_register() {
  Fixture f;
  f.vdp.frame_pending = 1;
  sms_vdp_set_sprite_flags(&f.vdp, 1, 0);
  check(f.status() == (0x80 | 0x40 | 0x1F), "status: frame + overflow + low bits %11111");
  check(f.vdp.frame_pending == 0 && f.vdp.sprite_overflow == 0, "a status read clears the flags");
  check(f.status() == 0x1F, "the second status read is clean");
  sms_vdp_set_sprite_flags(&f.vdp, 0, 1);
  f.vdp.line_pending = 1;
  check(f.status() == (0x20 | 0x1F), "collision is bit 5");
  check(f.vdp.line_pending == 0, "a status read also clears the line pending flag");
  check((f.status() & 0xE0) == 0, "all flags clear after the read");
}

void test_interrupt_timeline(unsigned r0, unsigned r1, unsigned r10, unsigned active, const char* label) {
  Fixture f;
  f.reg(0, static_cast<uint8_t>(r0));
  f.reg(1, static_cast<uint8_t>(r1));
  f.reg(10, static_cast<uint8_t>(r10));
  std::vector<unsigned> line_flags;
  std::vector<unsigned> frame_flags;
  for (uint64_t frame = 0; frame < 2; ++frame) {
    for (uint32_t l = 0; l < 262; ++l) {
      const uint8_t before_line = f.vdp.line_pending, before_frame = f.vdp.frame_pending;
      f.line(frame, l);
      if (frame == 1 && f.vdp.line_pending && !before_line) line_flags.push_back(l);
      if (frame == 1 && f.vdp.frame_pending && !before_frame) frame_flags.push_back(l);
      // a software acknowledge (status read) after every line, like an interrupt handler would
      if (f.vdp.line_pending || f.vdp.frame_pending) (void)f.status();
    }
  }
  std::vector<unsigned> expected;
  // independent rule: period R10 + 1 lines, first at R10, through line `active` inclusive; the counter is reloaded every
  // frame (contract section 9.4)
  for (unsigned l = r10; l <= active; l += r10 + 1) expected.push_back(l);
  check(line_flags == expected, std::string(label) + ": line interrupt flags at lines R10, 2R10+1, ... <= active");
  check(frame_flags.size() == 1 && frame_flags[0] == active + 1, std::string(label) + ": frame flag at the first line after the active display");
}

void test_interrupts() {
  test_interrupt_timeline(0x16, 0xE0, 15, 192, "192-line R10=15");
  test_interrupt_timeline(0x16, 0xE0, 0, 192, "192-line R10=0");
  test_interrupt_timeline(0x16, 0xE0, 255, 192, "192-line R10=255");
  test_interrupt_timeline(0x16, 0xE0, 191, 192, "192-line R10=191");
  test_interrupt_timeline(0x16, 0xE0, 192, 192, "192-line R10=192 (fires on line 192)");
  // 224-line mode: M4 M3 M2 M1 = 1 0 1 1 -> R0 bit 1, R1 bit 4
  test_interrupt_timeline(0x16, 0xF0, 15, 224, "224-line R10=15");

  // /INT gating by the enable bits, pending-but-disabled, acknowledge
  Fixture f;
  f.reg(0, 0x06);  // the reset R0 ($36) has IE1 set
  f.vdp.frame_pending = 1;
  f.vdp.line_pending = 1;
  check(sms_vdp_irq_sources(&f.vdp) == 0, "flags pending but both enables clear: /INT not asserted");
  f.reg(1, 0xA0);
  check(sms_vdp_irq_sources(&f.vdp) == SMS_IRQ_FRAME, "enabling R1 bit 5 asserts the frame source at once");
  f.reg(0, 0x36);  // IE1 set at reset value
  check(sms_vdp_irq_sources(&f.vdp) == (SMS_IRQ_FRAME | SMS_IRQ_LINE), "enabling R0 bit 4 asserts the line source");
  f.reg(1, 0x80);
  check(sms_vdp_irq_sources(&f.vdp) == SMS_IRQ_LINE, "clearing an enable bit deasserts its source");
  (void)f.status();
  check(sms_vdp_irq_sources(&f.vdp) == 0, "a status read deasserts /INT");
  f.reg(1, 0xA0);
  check(sms_vdp_irq_sources(&f.vdp) == 0, "enabling after the acknowledge asserts nothing");
  // a status read clears a pending line flag before the enable
  Fixture g;
  g.reg(0, 0x06);
  g.reg(10, 3);
  for (uint32_t l = 193; l < 262; ++l) g.line(0, l);  // reload from R10
  for (uint32_t l = 0; l < 4; ++l) g.line(1, l);
  check(g.vdp.line_pending == 1, "the line flag is set while IE1 is clear");
  (void)g.status();
  g.reg(0, 0x16);
  check(sms_vdp_irq_sources(&g.vdp) == 0, "acknowledged before the enable: nothing pending");
  // writing R10 affects only the next reload
  Fixture h;
  h.reg(0, 0x16);
  h.reg(10, 5);
  h.line(0, 0);  // counter 0xFF -> 0xFE (from reset); reloads do not happen outside the underflow
  h.reg(10, 1);
  check(h.vdp.line_counter == 0xFE, "writing R10 does not touch the running counter");
  for (uint32_t l = 193; l < 262; ++l) h.line(0, l);
  check(h.vdp.line_counter == 1, "the counter is reloaded from R10 outside the active display");
}

struct ModeRow {
  unsigned r0, r1;
  SmsVdpMode mode;
  const char* why;
};

void test_modes() {
  Fixture f;
  // (M4 M3 M2 M1) per MacDonald's table, with M4 = R0 bit 2, M3 = R1 bit 3, M2 = R0 bit 1, M1 = R1 bit 4
  const ModeRow rows[] = {
      {0x04, 0x00, SMS_VDP_MODE4_192, "1000"},  {0x06, 0x00, SMS_VDP_MODE4_192, "1010"},
      {0x04, 0x08, SMS_VDP_MODE4_192, "1100"},  {0x06, 0x18, SMS_VDP_MODE4_192, "1111"},
      {0x06, 0x10, SMS_VDP_MODE4_224, "1011"},  {0x06, 0x08, SMS_VDP_MODE_UNSUPPORTED, "1110 (240 lines)"},
      {0x04, 0x10, SMS_VDP_MODE_UNSUPPORTED, "1001 (invalid text)"}, {0x04, 0x18, SMS_VDP_MODE_UNSUPPORTED, "1101 (invalid text)"},
      {0x00, 0x00, SMS_VDP_MODE_UNSUPPORTED, "0000 (TMS9918)"}, {0x02, 0x08, SMS_VDP_MODE_UNSUPPORTED, "0110 (TMS9918)"},
      {0x05, 0x00, SMS_VDP_MODE_UNSUPPORTED, "R0 bit 0 (no sync)"},
  };
  for (const ModeRow& r : rows) {
    f.vdp.reg[0] = static_cast<uint8_t>(r.r0);
    f.vdp.reg[1] = static_cast<uint8_t>(r.r1);
    check(sms_vdp_mode(&f.vdp) == r.mode, std::string("mode ") + r.why);
  }
  // bits that do not select the mode never change it
  f.vdp.reg[0] = 0xFC;
  f.vdp.reg[1] = 0xE7;
  check(sms_vdp_mode(&f.vdp) == SMS_VDP_MODE4_192, "R0 bits 7-3 and R1 bits 7-5, 2-0 do not select the mode");
  check(sms_vdp_active_lines(SMS_VDP_MODE4_192) == 192 && sms_vdp_active_lines(SMS_VDP_MODE4_224) == 224 &&
            sms_vdp_active_lines(SMS_VDP_MODE_UNSUPPORTED) == 0,
        "active line counts");
}

void test_mode_stops() {
  {  // the mode is unobservable while the display and every interrupt are off: no stop
    Fixture f;
    f.reg(0, 0x04);
    f.reg(1, 0x90);  // M4 M3 M2 M1 = 1 0 0 1: invalid text mode, display off, frame IRQ off
    for (uint32_t l = 0; l < 262; ++l) f.line(0, l);
    check(f.mem.error == SMS_OK, "an unsupported mode with the display blanked does not stop");
  }
  {  // the display enabled: stops at the next line event
    Fixture f;
    f.reg(0, 0x04);
    f.reg(1, 0xD0);
    f.line(0, 1);
    check(f.mem.error == SMS_ERROR_VDP_MODE_UNSUPPORTED && f.mem.error_cycles == 228, "display enabled in an unsupported mode stops at the line event");
  }
  {  // an interrupt the mode would time stops as well
    Fixture f;
    f.reg(0, 0x04);
    f.reg(1, 0xB0);  // frame IRQ enable + M1
    f.line(0, 5);
    check(f.mem.error == SMS_ERROR_VDP_MODE_UNSUPPORTED, "an enabled interrupt in an unsupported mode stops");
  }
  {  // TMS9918 legacy (M4 clear) with the display on
    Fixture f;
    f.reg(0, 0x00);
    f.reg(1, 0xC0);
    f.line(0, 0);
    check(f.mem.error == SMS_ERROR_VDP_MODE_UNSUPPORTED, "the TMS9918 modes stop typed");
  }
  {  // a status read in an unsupported mode stops at the access T-state
    Fixture f;
    f.reg(0, 0x04);
    f.reg(1, 0x90);
    (void)f.status(77);
    check(f.mem.error == SMS_ERROR_VDP_MODE_UNSUPPORTED && f.mem.error_cycles == 77, "a status read in an unsupported mode stops at the read");
  }
  {  // V counter read likewise
    Fixture f;
    f.reg(0, 0x05);
    (void)f.vcount(5);
    check(f.mem.error == SMS_ERROR_VDP_MODE_UNSUPPORTED && f.mem.error_cycles == 5, "a V counter read in an unsupported mode stops");
  }
  {  // H counter: U3, never a guessed value
    Fixture f;
    (void)sms_vdp_read(&f.vdp, SMS_PORT_H_COUNTER, 9);
    check(f.mem.error == SMS_ERROR_HCOUNTER_UNRESOLVED && f.mem.error_address == 0x7F && f.mem.error_cycles == 9,
          "an H counter read stops with SMS_ERROR_HCOUNTER_UNRESOLVED");
  }
  {  // the first error wins
    Fixture f;
    f.reg(0, 0x04);
    f.reg(1, 0x90);
    (void)f.status(10);
    (void)sms_vdp_read(&f.vdp, SMS_PORT_H_COUNTER, 20);
    check(f.mem.error == SMS_ERROR_VDP_MODE_UNSUPPORTED && f.mem.error_cycles == 10, "the first latched error wins");
  }
}

void test_v_counter() {
  Fixture f;
  // NTSC 192-line: $00-$DA, then $D5-$FF (262 values) [cap:vdp.v_counter_ntsc]
  for (uint32_t l = 0; l < 262; ++l) {
    const unsigned expected = l <= 0xDA ? l : l - 6u;
    check(sms_vdp_v_counter_value(SMS_VDP_MODE4_192, l) == expected, "192-line V counter of line " + std::to_string(l));
  }
  check(sms_vdp_v_counter_value(SMS_VDP_MODE4_192, 219) == 0xD5 && sms_vdp_v_counter_value(SMS_VDP_MODE4_192, 261) == 0xFF, "192-line V counter endpoints");
  for (uint32_t l = 0; l < 262; ++l) {
    const unsigned expected = l <= 0xEA ? l : l - 6u;
    check(sms_vdp_v_counter_value(SMS_VDP_MODE4_224, l) == expected, "224-line V counter of line " + std::to_string(l));
  }
  f.line(0, 100);
  check(f.vcount() == 100, "a V counter read returns the current line's counter");
  f.line(0, 225);
  check(f.vcount() == 219, "a V counter read in the jump region");
}

void test_line_hook_and_trace() {
  Fixture f;
  struct Seen {
    std::vector<uint32_t> lines;
    uint8_t r10 = 0;
  } seen;
  f.vdp.line_hook = [](void* context, const SmsVdp* vdp, uint32_t line, uint64_t) {
    Seen* s = static_cast<Seen*>(context);
    s->lines.push_back(line);
    s->r10 = vdp->reg[10];
  };
  f.vdp.line_hook_context = &seen;
  f.reg(10, 7);
  for (uint32_t l = 0; l < 262; ++l) f.line(0, l);
  check(seen.lines.size() == 192 && seen.lines.front() == 0 && seen.lines.back() == 191, "the raster hook runs on the 192 active lines only");
  check(seen.r10 == 7, "the hook sees the register file as the line sees it");

  // trace: register writes, status reads, flag events, per-frame write summaries
  Fixture t;
  t.ctl(3, 10);
  t.ctl(0x8A, 11);  // R10 = 3 at T = 11 (the second byte completes the command)
  t.ctl(0x00, 100);
  t.ctl(0x40, 101);
  t.data(0xAB, 102);
  t.data(0xCD, 103);
  t.ctl(0x02, 104);
  t.ctl(0xC0, 105);
  t.data(0x2A, 106);
  check(t.status(200) == 0x1F, "trace fixture status");
  for (uint32_t l = 193; l < 262; ++l) t.line(0, l);  // reload the counter from R10 = 3 and set the frame flag at line 193
  for (uint32_t l = 0; l < 4; ++l) t.line(1, l);      // the line flag at line 3
  // frame 1 line 0 (T = 262 x 228) flushed the write summaries of frame 0 already
  const SmsVdpTraceEntry* e = t.trace.data();
  check(t.vdp.trace_count == 6, "trace entry count " + std::to_string(t.vdp.trace_count));
  if (t.vdp.trace_count == 6) {
    check(e[0].kind == SMS_VDP_TRACE_REG_WRITE && e[0].arg == 10 && e[0].byte == 3 && e[0].cycles == 11, "trace: register write with its cycle stamp");
    check(e[1].kind == SMS_VDP_TRACE_STATUS_READ && e[1].byte == 0x1F && e[1].cycles == 200, "trace: status read");
    check(e[2].kind == SMS_VDP_TRACE_FLAG_FRAME && e[2].arg == 193 && e[2].cycles == 193u * 228u, "trace: frame flag at line 193, T offset 0");
    check(e[3].kind == SMS_VDP_TRACE_VRAM_SUMMARY && e[3].arg == 2 && e[3].cycles == 262u * 228u, "trace: per-frame VRAM write summary count and stamp");
    check(e[4].kind == SMS_VDP_TRACE_CRAM_SUMMARY && e[4].arg == 1, "trace: per-frame CRAM write summary");
    check(e[5].kind == SMS_VDP_TRACE_FLAG_LINE && e[5].arg == 3 && e[5].cycles == (262u + 3u) * 228u, "trace: line flag at line 3 of the next frame");
    SmsSha256 sha;
    uint8_t out[32];
    sms_sha256_init(&sha);
    sms_sha256_update(&sha, t.vdp.vram, sizeof t.vdp.vram);
    sms_sha256_final(&sha, out);
    uint64_t first = 0;
    for (int i = 0; i < 8; ++i) first = (first << 8) | out[i];
    check(e[3].data == first, "trace: VRAM summary is the first 8 bytes of SHA-256(VRAM), big endian");
  }
  sms_vdp_flush_trace(&t.vdp, 1000);
  check(t.vdp.trace_count == 6, "a second flush with no writes adds nothing");

  // capacity: extra entries are counted, never written out of bounds
  Fixture c;
  sms_vdp_set_trace(&c.vdp, c.trace.data(), 2);
  for (int i = 0; i < 5; ++i) (void)c.status(static_cast<uint64_t>(i));
  check(c.vdp.trace_count == 2 && c.vdp.trace_dropped == 3, "trace overflow is counted");
}

void test_determinism_and_digest() {
  auto run = [](int variant) {
    Fixture f;
    f.setaddr(1, 0x0200);
    for (int i = 0; i < 40; ++i) f.data(static_cast<uint8_t>(i * 7));
    f.setaddr(3, 2);
    f.data(0x2B);
    f.reg(10, 9);
    if (variant == 1) f.data(0x01);
    for (uint32_t l = 0; l < 262; ++l) f.line(0, l);
    SmsSha256 sha;
    uint8_t out[32];
    sms_sha256_init(&sha);
    sms_vdp_digest(&f.vdp, &sha);
    sms_sha256_final(&sha, out);
    return std::string(reinterpret_cast<char*>(out), 32);
  };
  check(run(0) == run(0), "the VDP digest is deterministic");
  check(run(0) != run(1), "the VDP digest depends on the device state");
}

void test_palette_and_seam() {
  check(sms_vdp_cram_to_rgb888(0x00) == 0x000000 && sms_vdp_cram_to_rgb888(0x3F) == 0xFFFFFF, "palette black and white");
  check(sms_vdp_cram_to_rgb888(0x01) == 0x550000 && sms_vdp_cram_to_rgb888(0x02) == 0xAA0000 && sms_vdp_cram_to_rgb888(0x03) == 0xFF0000,
        "palette red steps (--BBGGRR, component x 85)");
  check(sms_vdp_cram_to_rgb888(0x0C) == 0x00FF00 && sms_vdp_cram_to_rgb888(0x30) == 0x0000FF && sms_vdp_cram_to_rgb888(0x1B) == 0xFFAA55,
        "palette green/blue positions and a mixed entry");
  check(sms_vdp_cram_to_rgb888(0xC0) == 0x000000, "palette bits 7-6 are ignored");

  SmsMachine m;
  static uint8_t rom[0x8000];
  check(sms_machine_init(&m, rom, sizeof rom, SMS_MAPPER_ROM_ONLY) == SMS_OK, "machine init");
  SmsVdp vdp;
  std::memset(&vdp, 0, sizeof vdp);
  sms_vdp_install(&m, &vdp);
  sms_machine_reset(&m);
  check(m.vdp.port.context == &vdp && m.vdp.port.read != nullptr && m.vdp.port.write != nullptr && m.vdp.port.reset != nullptr &&
            m.vdp.port.digest != nullptr && m.vdp.scanline != nullptr && m.vdp.irq_sources != nullptr,
        "the machine seam is fully wired");
  check(vdp.error_sink == &m.mem && vdp.reg[0] == 0x36 && vdp.line_counter == 0xFF, "install binds the error sink and the machine reset resets the VDP");
  vdp.frame_pending = 1;
  vdp.reg[1] = 0xA0;
  check(m.vdp.irq_sources(m.vdp.port.context) == SMS_IRQ_FRAME, "the seam reports the gated /INT sources");
}

}  // namespace

int main() {
  test_reset_state();
  test_control_and_data_ports();
  test_registers_and_cram();
  test_status_register();
  test_interrupts();
  test_modes();
  test_mode_stops();
  test_v_counter();
  test_line_hook_and_trace();
  test_determinism_and_digest();
  test_palette_and_seam();
  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("sms vdp: ok\n");
  return 0;
}
