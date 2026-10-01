// SEG-009-T009: Master System viewer core tests with fake clock/sleeper/poll/presenter/audio; never waits on real time.
// The CPU is replaced by a fake `z80_run` that retires 4-T steps, so the loop, input recording and pacing are checked without
// a generated image (the generated-native viewer/headless equivalence is sms_viewer_equivalence_test.py).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sms_viewer.h"

namespace {

extern "C" Z80Outcome z80_run(Z80Runtime* rt, uint64_t deadline) {
  rt->state.deadline = deadline;
  while (rt->state.cycles < rt->state.deadline && rt->outcome == Z80_OUTCOME_NONE) rt->state.cycles += 4;
  return rt->outcome;
}
int failures = 0;
void check(bool v, const std::string& m) {
  if (!v) {
    std::printf("FAIL: %s\n", m.c_str());
    ++failures;
  }
}

struct Fake {
  uint64_t now = 0;
  std::vector<uint64_t> sleeps;
  int polls = 0, quit_at = -1, reset_at = -1, audio_calls = 0, audio_result = 0;
  std::vector<SmsViewerInput> script;
  uint64_t audio_samples = 0;
};
uint64_t f_now(void* c) { return static_cast<Fake*>(c)->now; }
void f_sleep(void* c, uint64_t ns) {
  auto* f = static_cast<Fake*>(c);
  f->sleeps.push_back(ns);
  f->now += ns;
}
int f_poll(void* c, SmsViewerInput* in) {
  auto* f = static_cast<Fake*>(c);
  const int k = f->polls++;
  if (static_cast<size_t>(k) < f->script.size()) *in = f->script[static_cast<size_t>(k)];
  in->quit = k == f->quit_at;
  in->reset = k == f->reset_at;
  return 0;
}
int f_present(void*, const uint8_t*, uint32_t) { return 0; }
int f_audio(void* c, const int16_t*, uint32_t n) {
  auto* f = static_cast<Fake*>(c);
  ++f->audio_calls;
  f->audio_samples += n;
  return f->audio_result;
}
SmsViewerHost host_for(Fake& f) {
  SmsViewerHost h{};
  h.ctx = &f;
  h.now_ns = f_now;
  h.sleep_ns = f_sleep;
  h.poll = f_poll;
  h.present = f_present;
  h.audio = f_audio;
  return h;
}

SmsViewerRig rig;
const std::vector<uint8_t> rom(32768, 0);

void reset_rig() {
  SmsError e = SMS_OK;
  check(sms_viewer_rig_init(&rig, rom.data(), static_cast<uint32_t>(rom.size()), SMS_MAPPER_ROM_ONLY, &e), "rig init");
}

std::string digest_hex() {
  uint8_t d[32];
  char h[65];
  sms_machine_digest(&rig.machine, d);
  for (int i = 0; i < 32; ++i) std::snprintf(h + 2 * i, 3, "%02x", d[i]);
  return h;
}

SmsViewerInput in(uint8_t p1, uint8_t p2 = 0, uint8_t pause = 0) {
  SmsViewerInput i{};
  i.p1 = p1;
  i.p2 = p2;
  i.pause = pause;
  return i;
}

void test_pacer() {
  check(sms_pacer_period_floor_ns() == 16688152u, "period floor 59,736 T x 11 / 39,375,000 Hz = 16,688,152.7 ns");
  Fake f;
  SmsViewerHost h = host_for(f);
  SmsPacer p;
  sms_pacer_init(&p, 0);
  for (int i = 0; i < 1000; ++i) check(sms_pacer_wait(&p, &h) == 0, "pacer wait");
  const unsigned long long expect = static_cast<unsigned long long>(1000ull * 59736ull * 11ull * 1000000000ull / 39375000ull);
  check(p.deadline_ns == expect || p.deadline_ns == expect + 1u, "1000 periods accumulate the exact rational period (no drift)");
  check(p.sleep_calls == 999u, "every wait after the first sleeps to its deadline");
  SmsPacer u;
  sms_pacer_init(&u, 1);
  Fake f2;
  SmsViewerHost h2 = host_for(f2);
  h2.sleep_ns = nullptr;
  check(sms_pacer_wait(&u, &h2) == 0 && f2.sleeps.empty(), "unthrottled never sleeps and needs no sleeper");
  SmsPacer b;
  sms_pacer_init(&b, 0);
  Fake f3;
  SmsViewerHost h3 = host_for(f3);
  f3.now = 100;
  sms_pacer_wait(&b, &h3);
  f3.now = 50;
  check(sms_pacer_wait(&b, &h3) == -1, "a clock that goes backwards is rejected");
  Fake f4;
  SmsViewerHost h4 = host_for(f4);
  SmsPacer l;
  sms_pacer_init(&l, 0);
  sms_pacer_wait(&l, &h4);
  f4.now = 10u * sms_pacer_period_floor_ns();
  sms_pacer_wait(&l, &h4);
  check(l.resyncs == 1u && l.sleep_calls == 0u, "a long stall resynchronizes instead of bursting");
}

void test_keys_options() {
  SmsViewerKeys k{};
  k.up = k.button1 = k.right = 1;
  check(sms_viewer_pad_from_keys(&k) == (SMS_PAD_UP | SMS_PAD_RIGHT | SMS_PAD_BUTTON1), "key map");
  check(sms_viewer_pad_from_keys(nullptr) == 0u, "null keys release");
  SmsViewerOptions o;
  const char* a[] = {"--viewer-unthrottled", "--viewer-mute", "--viewer-frames", "12", "--viewer-scale", "2", "--viewer-record", "r.txt", "--other"};
  check(sms_viewer_options_parse(9, a, &o) == 0 && o.unthrottled && o.mute && o.frames == 12u && o.scale == 2u &&
            std::strcmp(o.record, "r.txt") == 0, "options parse");
  const char* bad1[] = {"--viewer-frames", "x"};
  const char* bad2[] = {"--viewer-scale", "0"};
  const char* bad3[] = {"--viewer-record"};
  check(sms_viewer_options_parse(2, bad1, &o) == -1 && sms_viewer_options_parse(2, bad2, &o) == -1 &&
            sms_viewer_options_parse(1, bad3, &o) == -1, "malformed options rejected");
  check(sms_viewer_options_parse(0, nullptr, &o) == 0 && !o.unthrottled && o.frames == SMS_NO_LIMIT, "defaults");
}

void test_run_and_recording() {
  reset_rig();
  Fake f;
  f.script = {in(0), in(SMS_PAD_UP), in(SMS_PAD_UP), in(SMS_PAD_UP | SMS_PAD_BUTTON1, 0, 1), in(0, 0, 1), in(0)};
  SmsViewerHost h = host_for(f);
  SmsPacer p;
  sms_pacer_init(&p, 1);
  SmsViewerResult r = sms_viewer_run(&rig, &h, &p, 8);
  check(r.outcome == SMS_VIEWER_FRAME_LIMIT && r.frames == 8u && f.polls == 8, "frame limit stops after 8 guest frames");
  check(rig.machine.rt.state.cycles >= 8u * SMS_CYCLES_PER_FRAME && rig.machine.rt.state.cycles < 8u * SMS_CYCLES_PER_FRAME + 8u,
        "one guest frame per iteration");
  // the input sampled before iteration k takes effect at the next frame start not yet applied: frame k + 1 (k >= 1), 0 for k = 0
  char text[1024];
  const size_t n = sms_viewer_format_script(&rig, text, sizeof text);
  check(std::string(text, n) == "2 U----- ------ -\n4 U---1- ------ P\n5 ------ ------ P\n6 ------ ------ -\n",
        std::string("recorded script: ") + std::string(text, n));
  check(n != 0 && sms_viewer_format_script(&rig, text, 5) == 0, "a too-small buffer is reported");
  SmsInputEvent parsed[8];
  uint32_t count = 0;
  check(sms_input_parse(text, n, parsed, 8, &count) == 0 && count == rig.event_count, "the recorded script parses");
  check(r.audio_samples > 0 && f.audio_samples == r.audio_samples, "audio drained every frame");
  const std::string live = digest_hex();
  // replay: the recorded events drive a fresh machine (no viewer) to the identical state digest
  reset_rig();
  sms_machine_set_input(&rig.machine, parsed, count);
  const SmsStop stop = sms_run_until_cycle(&rig.machine, 8u * SMS_CYCLES_PER_FRAME);
  sms_psg_sync(&rig.psg, stop.cycles);
  check(stop.kind == SMS_STOP_CYCLE && digest_hex() == live, "headless replay of the recorded script reproduces the digest");
}

void test_reset_quit_audio() {
  reset_rig();
  Fake f;
  f.script = {in(0), in(SMS_PAD_UP)};
  f.reset_at = 3;
  f.quit_at = 6;
  SmsViewerHost h = host_for(f);
  SmsPacer p;
  sms_pacer_init(&p, 1);
  SmsViewerResult r = sms_viewer_run(&rig, &h, &p, SMS_NO_LIMIT);
  check(r.outcome == SMS_VIEWER_QUIT && r.resets == 1u && r.frames == 6u, "quit after a reset");
  check(rig.machine.rt.state.cycles >= 3u * SMS_CYCLES_PER_FRAME && rig.machine.rt.state.cycles < 3u * SMS_CYCLES_PER_FRAME + 8u,
        "the reset restarted guest time (3 frames since the reset)");
  // a failing audio device, a NULL sink and a healthy one give the identical guest
  std::string digests[3];
  for (int mode = 0; mode < 3; ++mode) {
    reset_rig();
    Fake g;
    g.script = {in(0), in(SMS_PAD_BUTTON2, SMS_PAD_LEFT), in(0, 0, 1)};
    g.audio_result = mode == 1 ? -1 : 0;
    SmsViewerHost hg = host_for(g);
    if (mode == 2) hg.audio = nullptr;
    SmsPacer q;
    sms_pacer_init(&q, 1);
    const SmsViewerResult rr = sms_viewer_run(&rig, &hg, &q, 6);
    check(rr.outcome == SMS_VIEWER_FRAME_LIMIT, "run completes with any audio device state");
    if (mode == 1) check(rr.audio_failures == static_cast<uint64_t>(g.audio_calls) && g.audio_calls > 0, "failures are counted");
    if (mode == 2) check(rr.audio_failures == 0u && rr.audio_samples > 0u, "an absent sink still drains the ring");
    digests[mode] = digest_hex();
  }
  check(digests[0] == digests[1] && digests[0] == digests[2], "failing/absent audio leaves the guest digest identical");
  // host failures
  reset_rig();
  Fake e;
  SmsViewerHost he = host_for(e);
  he.poll = nullptr;
  SmsPacer q;
  sms_pacer_init(&q, 1);
  check(sms_viewer_run(&rig, &he, &q, 1).outcome == SMS_VIEWER_INVALID_ARGUMENT, "missing poll rejected");
}

}  // namespace

int main() {
  test_pacer();
  test_keys_options();
  test_run_and_recording();
  test_reset_quit_audio();
  if (failures == 0) std::printf("sms_viewer_tests: all passed\n");
  return failures == 0 ? 0 : 1;
}
