// SEG-009-T003: Master System machine composition, port decode, deterministic scheduler and interrupt wiring.
//
// The Z80 CPU is replaced by a scripted fake `z80_run` built on the REAL boundary/interrupt logic of z80_runtime.h
// (z80_boundary, z80_accept_int/nmi, the prefix-lock helpers): each "instruction" is a step with a T-state length and an
// action that performs host accesses at its instruction-start T-state, exactly like generated owners do. This checks the
// platform scheduler independently of any generated image. Expectations are derived from the machine and scheduling
// contracts (228 T lines, 262 lines, 59,736 T frames, IM1 13 T, NMI 11 T), never from the code under test.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "sms_machine.h"

namespace {

int failures = 0;
void check(bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::printf("FAIL: %s\n", message.c_str());
  }
}

// ---- scripted fake CPU ---------------------------------------------------------------------------------------------

struct Step {
  unsigned len = 4;
  std::function<void(Z80Runtime*)> action;
  uint16_t next = 0;
  bool halt = false;
  bool lock = false;
};
std::map<uint16_t, Step> program;

extern "C" Z80Outcome z80_run(Z80Runtime* rt, uint64_t deadline) {
  rt->state.deadline = deadline;
  for (;;) {
    const int boundary = z80_boundary(rt);
    if (boundary == 1) return rt->outcome;
    if (boundary == 2) continue;
    if (rt->state.in_prefix_run) {  // resume inside an endless prefix run: continue in 4-T steps to the deadline
      z80_lock_run(rt, rt->state.pc);
      return rt->outcome;
    }
    const auto it = program.find(rt->state.pc);
    if (it == program.end()) return rt->outcome = Z80_ERROR_NO_OWNER;
    const Step& step = it->second;
    if (step.lock) {
      if (z80_lock_enter(rt, rt->state.pc)) continue;
      z80_lock_run(rt, rt->state.pc);
      return rt->outcome;
    }
    rt->state.int_deferral = 0;
    if (step.action) step.action(rt);
    if (step.halt) rt->state.halted = 1;
    rt->state.pc = step.next;
    rt->state.cycles += step.len;
  }
}

// ---- stub devices --------------------------------------------------------------------------------------------------

struct Access {
  char kind;  // 'R' / 'W' / 'E' scanline event
  int cls;    // SmsPortClass, or line for events
  uint64_t cycles;
  uint8_t value;
  uint64_t event_cycles;  // last scanline event applied when the access happened
};

struct Devices {
  std::vector<Access> vdp, psg, pad;
  std::vector<std::pair<uint64_t, uint32_t>> events;  // (T, line within the frame), in application order
  std::vector<uint64_t> event_frames;
  std::vector<std::string> resets;
  uint64_t last_event = 0;
  uint8_t pending_frame = 0;
  uint8_t pending_line = 0;
  uint8_t next_read = 0x5A;
};
Devices dev;

uint8_t dev_read(std::vector<Access>& log, void* ctx, SmsPortClass cls, uint64_t cycles) {
  (void)ctx;
  log.push_back({'R', static_cast<int>(cls), cycles, dev.next_read, dev.last_event});
  return dev.next_read;
}
void dev_write(std::vector<Access>& log, SmsPortClass cls, uint8_t value, uint64_t cycles) {
  log.push_back({'W', static_cast<int>(cls), cycles, value, dev.last_event});
}
uint8_t vdp_read(void* c, SmsPortClass cls, uint64_t cycles) {
  const uint8_t v = dev_read(dev.vdp, c, cls, cycles);
  if (cls == SMS_PORT_VDP_STATUS) dev.pending_frame = dev.pending_line = 0;  // acknowledge
  return v;
}
void vdp_write(void*, SmsPortClass cls, uint8_t value, uint64_t cycles) { dev_write(dev.vdp, cls, value, cycles); }
void psg_write(void*, SmsPortClass cls, uint8_t value, uint64_t cycles) { dev_write(dev.psg, cls, value, cycles); }
uint8_t pad_read(void* c, SmsPortClass cls, uint64_t cycles) { return dev_read(dev.pad, c, cls, cycles); }
void pad_write(void*, SmsPortClass cls, uint8_t value, uint64_t cycles) { dev_write(dev.pad, cls, value, cycles); }
void vdp_reset(void*) { dev.resets.push_back("vdp"); }
void psg_reset(void*) { dev.resets.push_back("psg"); }
void pad_reset(void*) { dev.resets.push_back("pad"); }
void vdp_scanline(void*, uint64_t frame, uint32_t line, uint64_t cycles) {
  dev.events.push_back({cycles, line});
  dev.event_frames.push_back(frame);
  dev.last_event = cycles;
  if (line == 192u) dev.pending_frame = 1;
}
uint8_t vdp_irq(void*) {
  return static_cast<uint8_t>((dev.pending_frame ? SMS_IRQ_FRAME : 0u) | (dev.pending_line ? SMS_IRQ_LINE : 0u));
}

SmsMachine machine;
std::vector<uint8_t> rom(0x8000);
SmsIrqTraceEntry irq_buffer[4096];
SmsMapperTraceEntry mapper_buffer[256];

void attach_all() {
  machine.vdp.port.read = vdp_read;
  machine.vdp.port.write = vdp_write;
  machine.vdp.port.reset = vdp_reset;
  machine.vdp.scanline = vdp_scanline;
  machine.vdp.irq_sources = vdp_irq;
  machine.psg.write = psg_write;
  machine.psg.reset = psg_reset;
  machine.pad.read = pad_read;
  machine.pad.write = pad_write;
  machine.pad.reset = pad_reset;
}

void setup(bool devices) {
  program.clear();
  dev = Devices();
  check(sms_machine_init(&machine, rom.data(), static_cast<uint32_t>(rom.size()), SMS_MAPPER_ROM_ONLY) == SMS_OK, "init");
  if (devices) attach_all();
  sms_machine_set_traces(&machine, irq_buffer, 4096, mapper_buffer, 256);
  sms_machine_reset(&machine);
}

// A never-ending NOP loop at `pc` (4 T per instruction) and helper steps.
Step nop(uint16_t next, unsigned len = 4) {
  Step s;
  s.len = len;
  s.next = next;
  return s;
}
void nop_loop(uint16_t pc) { program[pc] = nop(pc); }
void out_step(uint16_t pc, uint16_t next, uint16_t port, uint8_t value, unsigned len) {
  Step s;
  s.len = len;
  s.next = next;
  s.action = [port, value](Z80Runtime* rt) { z80_io_out(rt, port, value); };
  program[pc] = s;
}
void in_step(uint16_t pc, uint16_t next, uint16_t port, unsigned len, uint8_t* sink = nullptr) {
  Step s;
  s.len = len;
  s.next = next;
  s.action = [port, sink](Z80Runtime* rt) {
    const uint8_t v = z80_io_in(rt, port);
    if (sink) *sink = v;
  };
  program[pc] = s;
}
// IM1 handler at 0x38 returning to `back`: IN A,(0xBF) (11) acknowledge, then RETI (14).
void im1_handler(uint16_t back) {
  in_step(0x38, 0x39, 0xBF, 11);
  Step reti = nop(back, 14);
  reti.action = [](Z80Runtime* rt) { rt->state.iff1 = rt->state.iff2 = 1; };
  program[0x39] = reti;
}
void nmi_handler(uint16_t back) {
  Step retn = nop(back, 14);
  retn.action = [](Z80Runtime* rt) { rt->state.iff1 = rt->state.iff2; };
  program[0x66] = retn;
}

// ---- tests ---------------------------------------------------------------------------------------------------------

std::string hex(const uint8_t* p, size_t n) {
  static const char d[] = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) {
    s += d[p[i] >> 4];
    s += d[p[i] & 15];
  }
  return s;
}

void test_sha256() {
  auto digest = [](const std::string& text) {
    SmsSha256 s;
    uint8_t out[32];
    sms_sha256_init(&s);
    sms_sha256_update(&s, text.data(), text.size());
    sms_sha256_final(&s, out);
    return hex(out, 32);
  };
  check(digest("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 empty");
  check(digest("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 abc");
  check(digest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
        "sha256 two-block message");
  // streaming in odd pieces equals one shot
  SmsSha256 s;
  uint8_t out[32];
  const std::string text(1000, 'x');
  sms_sha256_init(&s);
  for (size_t i = 0; i < text.size(); i += 7) sms_sha256_update(&s, text.data() + i, std::min<size_t>(7, text.size() - i));
  sms_sha256_final(&s, out);
  check(hex(out, 32) == digest(text), "sha256 streaming");
}

// Independent reference of the SMS 2 port map (MacDonald section 3), written as a table: [row A7A6][A0][write].
enum Ref { R_OPEN, R_IGN, R_MEM, R_IOC, R_PSG, R_VC, R_HC, R_VD, R_VCT, R_VS, R_DC, R_DD };
const int ref_table[4][2][2] = {
    {{R_OPEN, R_MEM}, {R_OPEN, R_IOC}},  // $00-$3F: read open, write memory control (even) / I/O control (odd)
    {{R_VC, R_PSG}, {R_HC, R_PSG}},      // $40-$7F: read V/H counter, write PSG
    {{R_VD, R_VD}, {R_VS, R_VCT}},       // $80-$BF: data / control+status
    {{R_DC, R_IGN}, {R_DD, R_IGN}},      // $C0-$FF: read $DC/$DD, write ignored
};
const SmsPortClass ref_class[] = {SMS_PORT_OPEN_READ,   SMS_PORT_IGNORED_WRITE, SMS_PORT_MEMORY_CONTROL, SMS_PORT_IO_CONTROL,
                                  SMS_PORT_PSG,         SMS_PORT_V_COUNTER,     SMS_PORT_H_COUNTER,      SMS_PORT_VDP_DATA,
                                  SMS_PORT_VDP_CONTROL, SMS_PORT_VDP_STATUS,    SMS_PORT_PAD_DC,         SMS_PORT_PAD_DD};
const SmsPortOwner ref_owner[] = {SMS_OWNER_NONE, SMS_OWNER_NONE, SMS_OWNER_MEMORY, SMS_OWNER_PAD, SMS_OWNER_PSG, SMS_OWNER_VDP,
                                  SMS_OWNER_VDP,  SMS_OWNER_VDP,  SMS_OWNER_VDP,    SMS_OWNER_VDP, SMS_OWNER_PAD, SMS_OWNER_PAD};

// Counts disagreements of a decode function with the reference over every 16-bit port and both directions.
unsigned decode_mismatches(const std::function<SmsPortClass(uint16_t, int)>& decode) {
  unsigned bad = 0;
  for (uint32_t port = 0; port < 0x10000u; ++port)
    for (int write = 0; write < 2; ++write) {
      const int row = (port >> 6) & 3;
      const int odd = port & 1;
      if (decode(static_cast<uint16_t>(port), write) != ref_class[ref_table[row][odd][write]]) ++bad;
    }
  return bad;
}

void test_port_decode() {
  check(decode_mismatches([](uint16_t p, int w) { return sms_port_decode(p, w); }) == 0, "port decode differs from the contract table");
  // mutation controls: a decode that ignores A6, or one that also honours A1, must be detected
  check(decode_mismatches([](uint16_t p, int w) { return sms_port_decode(static_cast<uint16_t>(p & ~0x40u), w); }) > 0, "mutant: A6 ignored not detected");
  check(decode_mismatches([](uint16_t p, int w) { return sms_port_decode(static_cast<uint16_t>(p & ~0x01u), w); }) > 0, "mutant: A0 ignored not detected");
  check(decode_mismatches([](uint16_t p, int w) { return sms_port_decode(static_cast<uint16_t>(p ^ ((p & 2u) << 5)), w); }) > 0, "mutant: A1 honoured as A6 not detected");
  // mirroring: only A7, A6, A0 matter
  for (uint32_t port = 0; port < 0x10000u; ++port)
    for (int w = 0; w < 2; ++w)
      if (sms_port_decode(static_cast<uint16_t>(port), w) != sms_port_decode(static_cast<uint16_t>(port & 0xC1u), w)) {
        check(false, "mirror mismatch at port " + std::to_string(port));
        return;
      }
  for (int c = 0; c < SMS_PORT_CLASS_COUNT; ++c) check(sms_port_owner(ref_class[c]) == ref_owner[c], "owner of class " + std::to_string(c));
}

void test_routing() {
  // Every decoded class reaches exactly one owner, for mirrored ports with arbitrary high bytes.
  for (uint32_t low = 0; low < 0x100u; ++low)
    for (int write = 0; write < 2; ++write) {
      setup(true);
      const uint16_t port = static_cast<uint16_t>(low | ((low * 37u) << 8));
      const SmsPortClass cls = sms_port_decode(port, write);
      const SmsPortOwner owner = sms_port_owner(cls);
      if (write) out_step(0, 0, port, 0x42, 11);
      else in_step(0, 0, port, 11);
      program[0].next = 1;
      nop_loop(1);
      machine.mem.memory_control = 0xABu;  // I/O chip enabled
      const SmsStop stop = sms_run_until_cycle(&machine, 40);
      const size_t v = dev.vdp.size(), p = dev.psg.size(), d = dev.pad.size();
      const size_t total = v + p + d;
      const bool memory_ok = machine.mem.error == SMS_OK;
      // memory control ($3E even) is accepted by the T002 owner ($42 has bit 6 set -> typed stop): routed to memory only
      if (owner == SMS_OWNER_MEMORY) {
        check(total == 0 && stop.kind == SMS_STOP_PLATFORM_ERROR && machine.mem.error == SMS_ERROR_CONTROL_BIT_UNSUPPORTED,
              "memory-control port " + std::to_string(low) + " must reach the memory owner only");
        continue;
      }
      check(memory_ok && stop.kind == SMS_STOP_CYCLE, "port " + std::to_string(port) + " raised an error");
      const size_t expect_v = owner == SMS_OWNER_VDP, expect_p = owner == SMS_OWNER_PSG, expect_d = owner == SMS_OWNER_PAD;
      check(v == expect_v && p == expect_p && d == expect_d, "port " + std::to_string(port) + " write=" + std::to_string(write) + " routed to the wrong owner(s)");
      if (total == 1) {
        const Access& a = v ? dev.vdp[0] : p ? dev.psg[0] : dev.pad[0];
        check(a.cls == static_cast<int>(cls) && a.cycles == 0, "class/T-state passed to the owner");
        check(write ? (a.kind == 'W' && a.value == 0x42) : a.kind == 'R', "direction/value passed to the owner");
      }
    }
}

void test_no_owner_and_io_disable() {
  // reads of $00-$3F return $FF, writes of $C0-$FF have no effect, without any device attached
  setup(false);
  uint8_t sink[4] = {0, 0, 0, 0};
  in_step(0, 1, 0x00, 11, &sink[0]);
  in_step(1, 2, 0xFF00 | 0x3E, 11, &sink[1]);  // high byte ignored
  out_step(2, 3, 0xC1, 0x99, 11);
  out_step(3, 4, 0x3E, 0xAF, 11);  // cartridge + RAM enabled, BIOS off, I/O chip disabled
  in_step(4, 5, 0xC0, 11, &sink[2]);  // I/O chip disabled: $FF even without a pad device
  in_step(5, 6, 0xDD, 11, &sink[3]);
  nop_loop(6);
  const SmsStop stop = sms_run_until_cycle(&machine, 100);
  check(stop.kind == SMS_STOP_CYCLE && machine.mem.error == SMS_OK, "no-owner accesses must not fail");
  check(sink[0] == 0xFF && sink[1] == 0xFF && sink[2] == 0xFF && sink[3] == 0xFF, "open reads return $FF");
  check(machine.mem.memory_control == 0xAF && sms_memory_io_disabled(&machine.mem), "port $3E write reached the memory owner");
  // with the I/O chip enabled the pad owner is consulted (and, absent, fails closed)
  setup(false);
  in_step(0, 1, 0xDC, 11);
  nop_loop(1);
  const SmsStop s2 = sms_run_until_cycle(&machine, 100);
  check(s2.kind == SMS_STOP_PLATFORM_ERROR && s2.sms_error == SMS_ERROR_PORT_UNIMPLEMENTED, "pad read without device fails closed");
}

void test_unimplemented() {
  struct Case {
    const char* name;
    bool write;
    uint16_t port;
  };
  const Case cases[] = {{"io control", true, 0x3F}, {"psg", true, 0x7F},      {"vdp data w", true, 0xBE},  {"vdp control", true, 0xBF},
                        {"v counter", false, 0x7E}, {"h counter", false, 0x7F}, {"vdp data r", false, 0xBE}, {"vdp status", false, 0xBF},
                        {"pad dc", false, 0xDC},    {"pad dd", false, 0xDD}};
  for (const Case& c : cases) {
    setup(false);
    // 8 NOPs (32 T), the offending access at T 32 (11 T), then a marker write that must never execute
    for (uint16_t i = 0; i < 8; ++i) program[i] = nop(static_cast<uint16_t>(i + 1));
    if (c.write) out_step(8, 9, c.port, 0x77, 11);
    else in_step(8, 9, c.port, 11);
    Step marker = nop(10, 10);
    marker.action = [](Z80Runtime* rt) { z80_write(rt, 0xC100, 0x11); };
    program[9] = marker;
    nop_loop(10);
    const SmsStop stop = sms_run_until_cycle(&machine, 1000);
    check(stop.kind == SMS_STOP_PLATFORM_ERROR && stop.sms_error == SMS_ERROR_PORT_UNIMPLEMENTED, std::string(c.name) + ": typed stop");
    check(stop.error_address == c.port && stop.error_cycles == 32 && (!c.write || stop.error_value == 0x77), std::string(c.name) + ": identity of the stop");
    check(stop.cycles == 43 && stop.pc == 9, std::string(c.name) + ": stops at the next instruction boundary");
    check(machine.mem.ram[0x100] == 0, std::string(c.name) + ": nothing ran after the offending instruction");
    check(!sms_stop_is_resumable(stop.kind), std::string(c.name) + ": fail-closed stop is not resumable");
    const SmsStop again = sms_run_until_cycle(&machine, 5000);
    check(std::memcmp(&again, &stop, sizeof stop) == 0 && machine.rt.state.cycles == 43, std::string(c.name) + ": permanent stop");
    const SmsStop again2 = sms_run_bounded(&machine, 9000, SMS_NO_LIMIT);
    check(std::memcmp(&again2, &stop, sizeof stop) == 0, std::string(c.name) + ": permanent stop through the bounded API");
  }
  // a device with only the write seam: its reads still fail closed
  setup(false);
  machine.vdp.port.write = vdp_write;
  in_step(0, 1, 0xBF, 11);
  nop_loop(1);
  check(sms_run_until_cycle(&machine, 100).sms_error == SMS_ERROR_PORT_UNIMPLEMENTED, "missing read seam fails closed");
  // memory errors from the T002 owner are platform errors too (write-through mapper control bit, port $3E)
  setup(true);
  machine.mem.family = SMS_MAPPER_SEGA;
  Step w;
  w.len = 13;
  w.next = 1;
  w.action = [](Z80Runtime* rt) { z80_write(rt, 0xFFFC, 0x10); };
  program[0] = w;
  nop_loop(1);
  const SmsStop stop = sms_run_until_cycle(&machine, 100);
  check(stop.kind == SMS_STOP_PLATFORM_ERROR && stop.sms_error == SMS_ERROR_CONTROL_BIT_UNSUPPORTED && stop.cycles == 13, "mapper control error");
  setup(true);
  out_step(0, 1, 0x3E, 0xE3, 11);
  nop_loop(1);
  const SmsStop stop2 = sms_run_until_cycle(&machine, 100);
  check(stop2.sms_error == SMS_ERROR_CONTROL_BIT_UNSUPPORTED && stop2.error_address == 0x3E && stop2.error_value == 0xE3, "port $3E error");
}

void test_reset() {
  setup(true);
  check((dev.resets == std::vector<std::string>{"vdp", "psg", "pad"}), "reset order is VDP, PSG, pad");
  machine.mem.ram[5] = 9;
  machine.io_control = 0x12;
  machine.rt.state.pc = 0x1234;
  machine.rt.state.iff1 = 1;
  dev.resets.clear();
  sms_machine_reset(&machine);
  check((dev.resets == std::vector<std::string>{"vdp", "psg", "pad"}), "reset order repeats");
  check(machine.mem.ram[0] == 0xAB && machine.mem.ram[5] == 0 && machine.io_control == 0xFF && machine.rt.state.pc == 0 &&
            machine.rt.state.iff1 == 0 && machine.rt.state.im == 0 && machine.rt.state.sp == 0xFFFF && machine.rt.state.a == 0xFF &&
            machine.mem.memory_control == 0xAB && machine.mem.mapper_regs[0] == 0 && machine.mem.mapper_regs[2] == 1 && machine.mem.mapper_regs[3] == 2 &&
            machine.rt.state.cycles == 0 && !machine.dead,
        "post-BIOS reset state");
  uint8_t a[32], b[32];
  sms_machine_digest(&machine, a);
  sms_machine_reset(&machine);
  sms_machine_digest(&machine, b);
  check(std::memcmp(a, b, 32) == 0, "reset is deterministic");
  machine.mem.ram[77] = 1;
  sms_machine_digest(&machine, b);
  check(std::memcmp(a, b, 32) != 0, "digest covers work RAM");
  machine.mem.ram[77] = 0;
  machine.io_control = 0;
  sms_machine_digest(&machine, b);
  check(std::memcmp(a, b, 32) != 0, "digest covers I/O control");
}

// Independent spec of the scanline event stream: event n happens at T = n x line length, line = n mod lines/frame.
unsigned geometry_mismatches(unsigned line_len, unsigned lines) {
  setup(true);
  nop_loop(0);
  sms_run_until_cycle(&machine, 2u * 59736u + 500u);
  unsigned bad = 0;
  for (size_t n = 0; n < dev.events.size(); ++n) {
    if (dev.events[n].first != n * line_len) ++bad;
    if (dev.events[n].second != n % lines) ++bad;
    if (dev.event_frames[n] != n / lines) ++bad;
  }
  if (dev.events.size() != (2u * 59736u + 500u) / line_len + 1u) ++bad;
  return bad;
}

void test_geometry() {
  check(geometry_mismatches(228, 262) == 0, "scanline events: 228 T lines, 262 lines per frame");
  check(SMS_CYCLES_PER_FRAME == 59736 && SMS_CYCLES_PER_LINE * SMS_LINES_PER_FRAME == SMS_CYCLES_PER_FRAME, "frame = 59,736 T");
  // mutation controls: an off-by-one line length or frame height disagrees with the platform's event stream
  check(geometry_mismatches(227, 262) > 0, "mutant: 227 T line not detected");
  check(geometry_mismatches(229, 262) > 0, "mutant: 229 T line not detected");
  check(geometry_mismatches(228, 261) > 0, "mutant: 261-line frame not detected");
  // frame boundary: run_until_frame stops at the first boundary with T >= n x 59,736
  setup(true);
  nop_loop(0);
  const SmsStop f = sms_run_until_frame(&machine, 2);
  check(f.kind == SMS_STOP_FRAME && f.cycles == 2 * 59736 && f.frame == 2, "run_until_frame(2)");
  // a 4-T NOP grid overshoots a target that is not a multiple of 4 by less than one instruction
  const SmsStop c = sms_run_until_cycle(&machine, 2 * 59736 + 3);
  check(c.kind == SMS_STOP_CYCLE && c.cycles == 2 * 59736 + 4, "run_until_cycle stops at the first boundary >= target");
  // a target already reached does no work
  const SmsStop c2 = sms_run_until_cycle(&machine, 10);
  check(c2.cycles == c.cycles, "a reached target does no work");
}

void test_halted_overshoot() {
  // a halted CPU fast-forwards in 4-T M1 cycles; events are applied in order, exactly once each
  setup(true);
  Step halt = nop(0, 4);
  halt.halt = true;
  program[0] = halt;
  const SmsStop stop = sms_run_until_cycle(&machine, 10 * 228 + 1);
  check(stop.cycles == 10 * 228 + 4, "halted time advances on the 4-T grid");
  check(dev.events.size() == 11 && dev.events.back().first == 10 * 228, "events are applied through T_event <= cycles, once each");
}

void test_straddle_ordering() {
  // U11: device state is advanced through every event with T_event <= T_access (instruction start) before the access.
  setup(true);
  for (uint16_t i = 0; i < 56; ++i) program[i] = nop(static_cast<uint16_t>(i + 1));  // T 0..223
  out_step(56, 57, 0x7F, 0xA1, 11);  // starts T 224 (bus write falls after the line start at 228): ordered BEFORE event 1
  out_step(57, 58, 0x7F, 0xA2, 11);  // starts T 235: after event 1
  // prefix chain: six superseded DD prefixes + OUT = 35 T starting at 446 (line start 456 lies inside), ordered before
  for (uint16_t i = 58; i < 58 + 50; ++i) program[i] = nop(static_cast<uint16_t>(i + 1));
  out_step(108, 109, 0x7F, 0xA3, 35);
  nop_loop(109);
  sms_run_until_cycle(&machine, 600);
  check(dev.psg.size() == 3, "three PSG accesses");
  if (dev.psg.size() == 3) {
    check(dev.psg[0].cycles == 224 && dev.psg[0].event_cycles == 0, "access at T 224 is ordered before the line start at 228");
    check(dev.psg[1].cycles == 235 && dev.psg[1].event_cycles == 228, "access at T 235 is ordered after the line start at 228");
    check(dev.psg[2].cycles == 446 && dev.psg[2].event_cycles == (dev.psg[2].cycles / 228) * 228, "chain access ordered at its instruction start");
  }
  // frame event: an access starting at T 59,732 (4 before the frame boundary) precedes frame 1 line 0
  setup(true);
  program[0] = nop(1, 59732);
  out_step(1, 2, 0x7F, 0x01, 23);
  nop_loop(2);
  sms_run_until_cycle(&machine, 60000);
  check(dev.psg.size() == 1 && dev.psg[0].cycles == 59732 && dev.psg[0].event_cycles == 59508, "frame-edge access precedes the frame start event");
  check(!dev.event_frames.empty() && dev.event_frames.back() == 1, "frame 1 started afterwards");
}

struct TraceRow {
  uint64_t cycles;
  int source, event;
  bool operator==(const TraceRow& o) const { return cycles == o.cycles && source == o.source && event == o.event; }
};
std::vector<TraceRow> trace_rows() {
  std::vector<TraceRow> rows;
  for (uint32_t i = 0; i < machine.irq_count; ++i) rows.push_back({irq_buffer[i].cycles, irq_buffer[i].source, irq_buffer[i].event});
  return rows;
}

void test_interrupts() {
  // INT level: frame interrupt asserted at line 192 (T 43,776), accepted at the next boundary (NOP grid: 43,776 itself),
  // IM1 response 13 T, deasserted by the handler's status read at 43,776 + 13.
  setup(true);
  nop_loop(0x100);
  machine.rt.state.pc = 0x100;
  machine.rt.state.im = 1;
  machine.rt.state.iff1 = machine.rt.state.iff2 = 1;
  im1_handler(0x100);
  const SmsStop stop = sms_run_until_cycle(&machine, 50000);
  check(stop.kind == SMS_STOP_CYCLE, "interrupt run stop");
  const uint64_t E = 192 * 228;
  const std::vector<TraceRow> want = {{E, SMS_IRQ_TRACE_FRAME, SMS_IRQ_ASSERTED},
                                      {E, SMS_IRQ_TRACE_FRAME, SMS_IRQ_ACCEPTED},
                                      {E + 13, SMS_IRQ_TRACE_FRAME, SMS_IRQ_DEASSERTED}};
  check(trace_rows() == want, "INT trace: asserted at the event, accepted at the boundary, deasserted by the status read");
  check(machine.rt.state.int_line == 0 && dev.vdp.size() == 1 && dev.vdp[0].cycles == E + 13, "status read at the handler's first instruction");
  check(machine.rt.state.iff1 == 1, "reti re-enabled interrupts");

  // SEG-009-T012: the acknowledge byte is always $FF, so IM0 (RST 38h) and IM2 (vector low byte $FF) are accepted without
  // the Z80 IM0 fail-closed outcome (im0_unsupported_acknowledge_byte is unreachable through the SMS mapping) and all
  // three modes reach the $0038 handler (IM2 through the word at (I << 8) | $FF).
  for (int mode = 0; mode <= 2; ++mode) {
    setup(true);
    nop_loop(0x100);
    machine.rt.state.pc = 0x100;
    machine.rt.state.im = static_cast<uint8_t>(mode);
    machine.rt.state.i = 0xC0;
    machine.mem.ram[0xFF] = 0x38;  // IM2 vector word at $C0FF = $0038
    machine.mem.ram[0x100] = 0x00;
    machine.rt.state.iff1 = machine.rt.state.iff2 = 1;
    im1_handler(0x100);
    const SmsStop mode_stop = sms_run_until_cycle(&machine, 50000);
    const std::vector<TraceRow> rows = trace_rows();
    check(mode_stop.kind == SMS_STOP_CYCLE && machine.rt.outcome != Z80_ERROR_IM0_UNSUPPORTED_ACKNOWLEDGE_BYTE &&
              !z80_outcome_is_error(machine.rt.outcome) && mode_stop.sms_error == SMS_OK,
          "IM" + std::to_string(mode) + ": $FF acknowledge is admissible, no error outcome");
    check(rows.size() == 3 && rows[1].event == SMS_IRQ_ACCEPTED && rows[2].event == SMS_IRQ_DEASSERTED && dev.vdp.size() == 1,
          "IM" + std::to_string(mode) + ": interrupt accepted and its handler at $0038 reached");
  }

  // IFF1 clear: the level stays asserted and is taken when interrupts are enabled (level semantics, no re-trigger)
  setup(true);
  nop_loop(0x100);
  machine.rt.state.pc = 0x100;
  machine.rt.state.im = 1;
  machine.rt.state.iff1 = 0;
  im1_handler(0x100);
  sms_run_until_cycle(&machine, 50000);
  check(machine.rt.state.int_line == 1 && machine.irq_count == 1, "level stays asserted while masked");
  machine.rt.state.iff1 = 1;
  sms_run_until_cycle(&machine, 50100);
  const std::vector<TraceRow> got = trace_rows();
  check(got.size() == 3 && got[1].event == SMS_IRQ_ACCEPTED && got[1].cycles >= 50000 && got[2].event == SMS_IRQ_DEASSERTED, "masked INT is accepted once enabled");

  // NMI: one edge per press at the frame start where the pause input goes '-' -> 'P'; holding does not repeat
  setup(true);
  nop_loop(0x100);
  machine.rt.state.pc = 0x100;
  nmi_handler(0x100);
  const SmsInputEvent events[] = {{0, 0, 0, 0}, {1, 0, 0, 1}, {2, 0, 0, 1}, {3, 0, 0, 0}, {5, 0, 0, 1}, {6, 0, 0, 0}};
  sms_machine_set_input(&machine, events, 6);
  sms_run_until_cycle(&machine, 7 * 59736);
  std::vector<uint64_t> accepted, asserted;
  for (const TraceRow& r : trace_rows()) {
    if (r.source == SMS_IRQ_TRACE_PAUSE && r.event == SMS_IRQ_ACCEPTED) accepted.push_back(r.cycles);
    if (r.source == SMS_IRQ_TRACE_PAUSE && r.event == SMS_IRQ_ASSERTED) asserted.push_back(r.cycles);
  }
  check(asserted == std::vector<uint64_t>({59736, 5 * 59736}), "pause edges at the frame starts of the presses");
  check(accepted.size() == 2 && accepted[0] >= 59736 && accepted[0] < 59736 + 4 && accepted[1] >= 5 * 59736 && accepted[1] < 5 * 59736 + 4,
        "NMI accepted at the first boundary after its edge");
  // an NMI response costs 11 T, so after the accepted edge the handler's RETN starts at accepted + 11 (PC 0x66)
  check(machine.rt.state.nmi_pending == 0, "NMI latch consumed");

  // nothing is accepted while in_prefix_run: INT level and NMI edge stay pending across an endless prefix run
  setup(true);
  Step lock;
  lock.lock = true;
  program[0x200] = lock;
  machine.rt.state.pc = 0x200;
  machine.rt.state.im = 1;
  machine.rt.state.iff1 = machine.rt.state.iff2 = 1;
  const SmsInputEvent press[] = {{1, 0, 0, 1}};
  sms_machine_set_input(&machine, press, 1);
  const SmsStop locked = sms_run_until_cycle(&machine, 70000);
  check(locked.kind == SMS_STOP_CYCLE && machine.rt.outcome == Z80_OUTCOME_PREFIX_LOCK && machine.rt.state.in_prefix_run == 1, "prefix_lock is resumable");
  bool accepted_any = false;
  for (const TraceRow& r : trace_rows())
    if (r.event == SMS_IRQ_ACCEPTED) accepted_any = true;
  check(!accepted_any && machine.rt.state.nmi_pending == 1 && machine.rt.state.int_line == 1, "INT/NMI are not accepted inside a prefix run");
}

void test_halt_idle_and_budget() {
  setup(true);
  Step halt = nop(0, 4);
  halt.halt = true;
  program[0] = halt;
  machine.stop_on_halt_idle = 1;
  const SmsStop idle = sms_run_until_cycle(&machine, 1000000);
  check(idle.kind == SMS_STOP_HALT_IDLE && sms_stop_is_resumable(idle.kind), "halt idle with interrupts disabled is a resumable diagnostic");
  // a pending pause press is a wake source: not idle
  setup(true);
  program[0] = halt;
  nmi_handler(1);
  program[1] = halt;
  machine.stop_on_halt_idle = 1;
  const SmsInputEvent press[] = {{2, 0, 0, 1}};
  sms_machine_set_input(&machine, press, 1);
  check(sms_run_until_cycle(&machine, 100000).kind == SMS_STOP_CYCLE, "a scripted pause keeps a halted CPU non-idle");

  // budgets are resumable reports; the frame bound wins a coincident bound
  setup(true);
  nop_loop(0);
  SmsStop s = sms_run_bounded(&machine, 100000, SMS_NO_LIMIT);
  check(s.kind == SMS_STOP_CYCLE_BUDGET && sms_stop_is_resumable(s.kind) && s.cycles == 100000, "cycle budget stop");
  s = sms_run_bounded(&machine, 200000, 1);
  check(s.kind == SMS_STOP_FRAME && s.cycles == 100000, "frame 1 already reached: frame bound reported");  // 59,736 < 100,000
  s = sms_run_bounded(&machine, 59736 * 3, 3);
  check(s.kind == SMS_STOP_FRAME && s.cycles == 3 * 59736, "coincident bounds: frame wins");
  s = sms_run_bounded(&machine, 59736 * 3 + 40, 10);
  check(s.kind == SMS_STOP_CYCLE_BUDGET && s.cycles == 3 * 59736 + 40, "budget before the frame bound");
  s = sms_run_bounded(&machine, SMS_NO_LIMIT, 4);
  check(s.kind == SMS_STOP_FRAME && s.cycles == 4 * 59736, "resume after a budget report");
}

void test_mapper_trace() {
  setup(false);
  machine.mem.family = SMS_MAPPER_SEGA;  // the trace follows writes to $FFFC-$FFFF of a Sega-mapper cartridge
  Step a;
  a.len = 13;
  a.next = 1;
  a.action = [](Z80Runtime* rt) { z80_write(rt, 0xFFFE, 3); };
  program[0] = a;
  Step b = a;
  b.next = 2;
  b.action = [](Z80Runtime* rt) { z80_write(rt, 0xFFFD, 2); };
  program[1] = b;
  Step c = a;
  c.next = 3;
  c.action = [](Z80Runtime* rt) { z80_write(rt, 0xDFFF, 7); };  // RAM copy only: not a mapper write
  program[2] = c;
  nop_loop(3);
  sms_run_until_cycle(&machine, 100);
  check(machine.mapper_count == 2 && mapper_buffer[0].reg == 2 && mapper_buffer[0].value == 3 && mapper_buffer[0].cycles == 0 &&
            mapper_buffer[1].reg == 1 && mapper_buffer[1].value == 2 && mapper_buffer[1].cycles == 13,
        "mapper trace records register writes at their T-state");
  machine.mem.family = SMS_MAPPER_ROM_ONLY;
}

// A mixed program: NOP work, PSG/pad/VDP accesses, frame INT handler, NMI. Used for split-run equivalence.
void mixed_program() {
  nop_loop(0x100);
  program[0x100] = nop(0x101, 4);
  out_step(0x101, 0x102, 0x7F, 0x55, 11);
  in_step(0x102, 0x103, 0x7E, 11);
  out_step(0x103, 0x104, 0xBF, 0x81, 11);
  Step w = nop(0x100, 13);
  w.action = [](Z80Runtime* rt) { z80_write(rt, 0xC010, static_cast<uint8_t>(rt->state.cycles)); };
  program[0x104] = w;
  im1_handler(0x100);
  nmi_handler(0x100);
  machine.rt.state.pc = 0x100;
  machine.rt.state.im = 1;
  machine.rt.state.iff1 = machine.rt.state.iff2 = 1;
}

void test_split_equivalence() {
  static const SmsInputEvent script[] = {{1, 0x11, 0, 1}, {3, 0, 0, 0}, {4, 0, 0, 1}, {5, 0, 0, 0}};
  auto run = [&](const std::vector<uint64_t>& slices, uint64_t final_target, uint8_t out[32], std::vector<TraceRow>& trace) {
    setup(true);
    mixed_program();
    sms_machine_set_input(&machine, script, 4);
    for (uint64_t t : slices) sms_run_until_cycle(&machine, t);
    const SmsStop s = sms_run_until_cycle(&machine, final_target);
    sms_machine_digest(&machine, out);
    trace = trace_rows();
    return s.cycles;
  };
  const uint64_t target = 6 * 59736 + 123;
  uint8_t base[32], other[32];
  std::vector<TraceRow> base_trace, other_trace;
  const uint64_t base_cycles = run({}, target, base, base_trace);
  check(base_trace.size() > 10, "mixed program produced interrupt traffic");
  uint64_t state = 0x1234;
  for (int seed = 0; seed < 25; ++seed) {
    std::vector<uint64_t> slices;
    uint64_t t = 0;
    while (t < target) {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      t += 1 + (state >> 33) % (seed % 2 ? 100 : 70000);
      if (t < target) slices.push_back(t);
      if (slices.size() > 4000) break;
    }
    const uint64_t cycles = run(slices, target, other, other_trace);
    check(cycles == base_cycles && std::memcmp(base, other, 32) == 0 && base_trace == other_trace, "split run seed " + std::to_string(seed) + " differs from the straight run");
  }
  // slicing through every event boundary and T-state around it
  std::vector<uint64_t> edges;
  for (uint64_t e = 228; e < 4 * 228; e += 228)
    for (int d = -2; d <= 2; ++d) edges.push_back(static_cast<uint64_t>(static_cast<int64_t>(e) + d));
  check(run(edges, target, other, other_trace) == base_cycles && std::memcmp(base, other, 32) == 0, "slices around event boundaries");
}

}  // namespace

int main() {
  rom.assign(rom.size(), 0);
  test_sha256();
  test_port_decode();
  test_routing();
  test_no_owner_and_io_disable();
  test_unimplemented();
  test_reset();
  test_geometry();
  test_halted_overshoot();
  test_straddle_ordering();
  test_interrupts();
  test_halt_idle_and_budget();
  test_mapper_trace();
  test_split_equivalence();
  {
    // scripted input parser
    SmsInputEvent ev[8];
    uint32_t n = 0;
    const char good[] = "# c\n0 ------ ------ -\n 12\tU-LR12 -D---- P # hold\n\n12 ------ ------ -\r\n";
    check(sms_input_parse(good, sizeof good - 1, ev, 8, &n) == 0 && n == 3 && ev[1].frame == 12 && ev[1].p1 == 0x3D && ev[1].p2 == SMS_PAD_DOWN && ev[1].pause == 1 && ev[2].pause == 0,
          "input script parse");
    const char* bad[] = {"x ------ ------ -\n", "0 UDLR1 ------ -\n", "0 ------ ------ Q\n", "2 ------ ------ -\n1 ------ ------ -\n",
                         "0 ------ ------ - junk\n", "0 ------ -----\n", "0 ------ ------ -\n0 ------ ------ -\n0 ------ ------ -\n"};
    for (const char* text : bad) {
      const unsigned line = sms_input_parse(text, std::strlen(text), ev, text == bad[6] ? 2 : 8, &n);
      check(line != 0, std::string("malformed script accepted: ") + text);
    }
  }
  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("sms runtime: ok\n");
  return 0;
}
