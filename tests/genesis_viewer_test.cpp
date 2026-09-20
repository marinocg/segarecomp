// SEG-007-T254: viewer core tests with fake clock/sleeper/presenter; never waits on real time.
#include "viewer.h"
#include "vdp_render.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
int failures = 0;
void check(bool v, const char *m) { if (!v) { std::printf("FAIL: %s\n", m); ++failures; } }

uint32_t g_limit = 0, g_count = 0;
GenesisControlTransfer dispatch_fn(GenesisRuntime *r) {
  if (g_count++ >= g_limit) { GenesisControlTransfer t{}; t.kind = GENESIS_COMPLETE; return t; }
  return genesis_runtime_retire_m68k_instruction(r, 6400u, r->pc + 2u);
}

void setup(GenesisRuntime &r) {
  r.pc = 0x200u; r.sr = 0x2000u;
  uint16_t *g = r.devices.vdp.registers;
  g[1] = 0x04; g[2] = 0x30; g[4] = 0x04; g[5] = 0x28; g[11] = 0x04; g[12] = 0x81; g[16] = 0x01;
  r.devices.vdp.cram[0] = 0x0Eu;
}

struct Obs { GenesisFrameArtifact frame; GenesisLiveFrameObserver o; };
void attach(Obs &s, GenesisRuntime &r) {
  std::memset(&s, 0, sizeof(s));
  s.o.producer = genesis_vdp_produce_frame; s.o.latest = &s.frame; r.live_frame_observer = &s.o;
}

struct Fake {
  uint64_t now = 0; std::vector<uint64_t> sleeps; int closed_after = -1, polls = 0, presents = 0;
  int fail_present = 0; std::vector<uint64_t> advance; size_t ai = 0;
};
uint64_t f_now(void *c) { auto *f = (Fake *)c; uint64_t n = f->now; if (f->ai < f->advance.size()) f->now += f->advance[f->ai++]; return n; }
void f_sleep(void *c, uint64_t ns) { auto *f = (Fake *)c; f->sleeps.push_back(ns); f->now += ns; }
int f_present(void *c, const GenesisFrameArtifact *) { auto *f = (Fake *)c; ++f->presents; return f->fail_present; }
int f_closed(void *c) { auto *f = (Fake *)c; return f->closed_after >= 0 && f->polls++ >= f->closed_after; }
GenesisViewerHost host(Fake &f) { return {&f, f_now, f_sleep, f_present, f_closed}; }

void pacer_tests() {
  Fake f; auto h = host(f); GenesisPacer p; genesis_pacer_init(&p, 0);
  uint64_t per = genesis_pacer_period_floor_ns();
  check(per == 16688154u, "NTSC period floor");
  check(genesis_pacer_wait(&p, &h) == 0 && f.sleeps.empty(), "first frame immediate");
  check(genesis_pacer_wait(&p, &h) == 0 && f.sleeps.size() == 1 && f.sleeps[0] == per, "early frame sleeps a full period to deadline");
  uint64_t t0 = f.now;
  f.now = t0 + 1000000u; // half-period early? on-time within the period
  size_t before = f.sleeps.size();
  check(genesis_pacer_wait(&p, &h) == 0 && f.sleeps.size() == before + 1, "sleeps accumulated deadline (no reset from wake)");
  // repeated late (within threshold): no sleep, deadline advances
  f.now += 2 * per; before = f.sleeps.size();
  check(genesis_pacer_wait(&p, &h) == 0 && f.sleeps.size() == before && p.resyncs == 0, "late within threshold: no sleep/resync");
  // stall beyond threshold: resync once, then paced from new base
  f.now += 100 * per;
  check(genesis_pacer_wait(&p, &h) == 0 && p.resyncs == 1, "late stall resyncs");
  before = f.sleeps.size();
  check(genesis_pacer_wait(&p, &h) == 0 && f.sleeps.size() == before + 1 && f.sleeps.back() <= per, "no catch-up burst after resync");
  // backwards clock fails closed, no mutation
  GenesisPacer snap = p; f.now = 0;
  check(genesis_pacer_wait(&p, &h) == -1 && std::memcmp(&snap, &p, sizeof(p)) == 0, "non-monotonic clock rejected");
  // unthrottled
  Fake g; auto hg = host(g); GenesisPacer u; genesis_pacer_init(&u, 1);
  for (int i = 0; i < 5; ++i) (void)genesis_pacer_wait(&u, &hg);
  check(g.sleeps.empty() && u.sleep_calls == 0, "unthrottled never sleeps");
  GenesisViewerHost bad = {nullptr, nullptr, nullptr, nullptr, nullptr};
  check(genesis_pacer_wait(&p, &bad) == -1 && genesis_pacer_wait(nullptr, &h) == -1, "null args fail");
}

void options_tests() {
  GenesisViewerOptions o; const char *a[] = {"--viewer-unthrottled", "--viewer-slice", "16"};
  check(genesis_viewer_options_parse(3, a, &o) == 0 && o.unthrottled && o.slice_dispatches == 16, "parse ok");
  const char *z[] = {"--viewer-slice", "0"}; check(genesis_viewer_options_parse(2, z, &o) == -1, "zero slice");
  const char *m[] = {"--viewer-slice"}; check(genesis_viewer_options_parse(1, m, &o) == -1, "missing value");
  const char *n[] = {"--viewer-slice", "12x"}; check(genesis_viewer_options_parse(2, n, &o) == -1, "malformed");
  const char *ov[] = {"--viewer-slice", "99999999999"}; check(genesis_viewer_options_parse(2, ov, &o) == -1, "overflow");
  check(genesis_viewer_options_parse(0, nullptr, &o) == 0 && !o.unthrottled && o.slice_dispatches == GENESIS_VIEWER_DEFAULT_SLICE, "defaults");
}

struct Run { GenesisRuntime r{}; Obs s; };
GenesisViewerResult go(Run &run, uint32_t slice, bool unthrottled, Fake &f, uint64_t total, uint32_t limit) {
  std::memset(&run.r, 0, sizeof(run.r)); setup(run.r); attach(run.s, run.r);
  g_count = 0; g_limit = limit;
  GenesisViewerOptions o{(uint8_t)unthrottled, slice}; auto h = host(f); GenesisPacer p; genesis_pacer_init(&p, unthrottled);
  return genesis_viewer_run(&run.r, dispatch_fn, &o, &h, &p, total);
}

void run_tests() {
  Fake a, b, c; Run ra, rb, rc;
  auto x = go(ra, 7, false, a, 1000000, 200);
  auto y = go(rb, 100000, true, b, 1000000, 200);
  check(x.outcome == GENESIS_VIEWER_GUEST_COMPLETE && y.outcome == GENESIS_VIEWER_GUEST_COMPLETE, "guest complete");
  ra.r.live_frame_observer = nullptr; rb.r.live_frame_observer = nullptr;
  check(std::memcmp(&ra.r, &rb.r, sizeof(ra.r)) == 0, "sliced vs unsliced guest state identical");
  check(ra.s.o.sequence == rb.s.o.sequence && std::memcmp(&ra.s.frame, &rb.s.frame, sizeof(ra.s.frame)) == 0, "frame artifacts identical");
  check(x.slices > y.slices && b.sleeps.empty(), "slices resume; unthrottled no sleep");
  check(a.presents >= 1, "frames presented");
  auto e = go(rc, 10, false, c, 25, 200);
  check(e.outcome == GENESIS_VIEWER_RUNNER_EXHAUSTED && e.dispatches == 25, "runner exhaustion disjoint from stop/yield");
  Fake w; w.closed_after = 2; Run rw;
  auto cl = go(rw, 5, false, w, 1000000, 200);
  check(cl.outcome == GENESIS_VIEWER_WINDOW_CLOSED && cl.slices == 2, "window closure stops");
  Fake pf; pf.fail_present = 1; Run rp;
  check(go(rp, 100, false, pf, 1000000, 200).outcome == GENESIS_VIEWER_HOST_ERROR, "present failure is host error");
  Fake nf; Run rn; auto nn = go(rn, 4, false, nf, 1000000, 3);
  check(nn.outcome == GENESIS_VIEWER_GUEST_COMPLETE && nf.presents == 0 && nn.frames_presented == 0, "no frame -> nothing presented");
  Run rz; Fake zf; auto zr = go(rz, 0, false, zf, 10, 5);
  check(zr.outcome == GENESIS_VIEWER_INVALID_ARGUMENT, "zero slice rejected");
  GenesisRuntime nobs{}; GenesisViewerOptions o{0, 4}; auto h = host(zf); GenesisPacer p; genesis_pacer_init(&p, 0);
  check(genesis_viewer_run(&nobs, dispatch_fn, &o, &h, &p, 10).outcome == GENESIS_VIEWER_INVALID_ARGUMENT, "missing observer rejected");
}
}  // namespace

int main() { pacer_tests(); options_tests(); run_tests(); return failures ? 1 : 0; }
