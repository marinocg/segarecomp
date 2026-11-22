// SEG-152-T001: scenery window sufficiency. Can ABI 2's bounded scenery windows (ADR 0309/0313/0314: <= 2 windows per plane, <= 128 x 64 cells, <= 4096 words, looked up by the host with the line's
// actual scroll, generic fallback per plane) reproduce every scenery answer an ABI 1 provider gives? Each scenario is an original synthetic scene (no ROM, no vendor data) reproducing one ABI 1 shape,
// run through the shared twin-provider harness (abi_parity_harness.hpp) over the real extension producer at 16:9, 21:9 and 32:9, over the asymmetric placement matrix of ADR 0315 (left-aligned,
// intermediate, centred, right-aligned), and under both timings (the exact-frame view and the render(N) prediction). Every scenario is classified PASS (pixel-for-pixel identical rows) or
// LIMIT:<class> with a deterministic failing assertion (the differing rows exist and each is attributed to the class); a builder that used another placement than it declared is
// PROVIDER_ERROR:placement and never a limit. The output lines `PARITY ...` are the evidence tabulated in docs/architecture/widescreen-scenery-window-sufficiency.md.
#include "abi_parity_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace {
using namespace parity;

int failures = 0;
void check(bool ok, const std::string &what) {
  if (!ok) { std::printf("FAIL: %s\n", what.c_str()); ++failures; }
}

struct Aspect { const char *name; int side; };
const Aspect kAspects[] = {{"16:9", 39}, {"21:9", 101}, {"32:9", 238}};   // the planner's side for a 320 x 224 picture (tests/vsprite_model.py side_of): floor(224 n / d) - 320, halved

// Sets plane `p` from the game's unwrapped scroll u(row) (negative = scrolled right): the VDP holds wrap10(u) and the provider's world offset is -u (+ 512 e): k = (hs - u + 512 e) / 512.
void set_plane(Spec &s, unsigned p, const std::function<int(int)> &u, const std::function<int(int)> &extra = nullptr, const std::function<int(int)> &speed = nullptr) {
  for (int r = 0; r < H; ++r) {
    const int unwrapped = u(r);
    s.hs[p][r] = wrap10(unwrapped);
    s.k[p][r] = (s.hs[p][r] - unwrapped) / 512 + (extra ? extra(r) : 0);
    s.speed[p][r] = speed ? speed(r) : 0;
  }
}

struct Scenario {
  Spec spec;
  // The expected class per aspect index (0 16:9, 1 21:9, 2 32:9) under Exact and Predicted timing; independent of the placement (the matrix asserts that it is).
  std::string exact[3], predicted[3];
  std::string why;   // the reason in one line (documented in the architecture note)
};
Scenario make(const char *id, const char *why) { Scenario sc; sc.spec.id = id; sc.why = why; for (int i = 0; i < 3; ++i) sc.exact[i] = sc.predicted[i] = "PASS"; return sc; }

std::vector<Scenario> scenarios() {
  std::vector<Scenario> v;
  {   // constant-scroll playfield
    Scenario s = make("constant", "one rectangle per plane");
    set_plane(s.spec, 0, [](int) { return -300; }); set_plane(s.spec, 1, [](int) { return -150; });
    v.push_back(s);
  }
  {   // few parallax bands: a monotone staircase, the union fits
    Scenario s = make("parallax-few", "6 bands, one rectangle");
    set_plane(s.spec, 0, [](int) { return -300; }); set_plane(s.spec, 1, [](int r) { return -(100 + (r / 38) * 40); });
    v.push_back(s);
  }
  {   // many parallax bands with a moderate spread: the hull of both extensions is too wide for one window at 32:9, left | right fits
    Scenario s = make("parallax-many", "28 bands, 28 x 24 px spread: left | right rectangles");
    set_plane(s.spec, 0, [](int) { return -300; }); set_plane(s.spec, 1, [](int r) { return -(100 + (r / 8) * 24); });
    v.push_back(s);
  }
  {   // interleaved distant scrolls: alternating cell rows scroll by -500 and +500 px (the widest spread a 10-bit scroll value can report): no contiguous row split fits, two full-height column windows do
    Scenario s = make("parallax-interleaved", "alternating cell rows at the two ends of the scroll range: two column windows");
    set_plane(s.spec, 0, [](int) { return -300; }); set_plane(s.spec, 1, [](int r) { return ((r / 8) & 1) != 0 ? 500 : -500; });
    v.push_back(s);
  }
  {   // split scroll: a different background camera in the lower region (the HUD-free status band keeps its own)
    Scenario s = make("split-scroll", "two row groups, each one rectangle");
    set_plane(s.spec, 0, [](int r) { return r < 160 ? -700 : -20; }); set_plane(s.spec, 1, [](int r) { return r < 160 ? -350 : -40; });
    v.push_back(s);
  }
  {   // authored here, generic there (S3K own_plane) on separate cell rows: the VDP's own word is reproduced by a window word copied from the VDP name table
    Scenario s = make("fallback-cell-rows", "own_plane lines on their own cell rows");
    set_plane(s.spec, 0, [](int) { return -300; }); set_plane(s.spec, 1, [](int r) { return -(100 + (r / 56) * 30); });
    for (int r = 160; r < H; ++r) s.spec.fallback[1][r] = 1;
    v.push_back(s);
  }
  {   // the fallback boundary falls inside a cell row: one plane coordinate has the VDP's word on one line and the authored word on the next
    Scenario s = make("fallback-mid-cell", "own_plane boundary inside a plane cell row");
    set_plane(s.spec, 0, [](int) { return -300; }); set_plane(s.spec, 1, [](int) { return -150; });
    for (int r = 100; r < H; ++r) s.spec.fallback[1][r] = 1;
    for (int i = 0; i < 3; ++i) s.exact[i] = s.predicted[i] = "LIMIT:per-line-fallback";
    v.push_back(s);
  }
  {   // a background that needs a registered origin multiple (S3K bg_register): the window sits at the VDP coordinates, the words come from the shifted world
    Scenario s = make("origin-multiple", "constant registered multiple k = 1");
    set_plane(s.spec, 0, [](int) { return -300; }); set_plane(s.spec, 1, [](int) { return -150; }, [](int) { return 1; });
    v.push_back(s);
  }
  {   // the multiple changes inside a plane cell row: the same plane coordinate maps to two world cells
    Scenario s = make("origin-multiple-mid-cell", "multiple changes inside a plane cell row");
    set_plane(s.spec, 0, [](int) { return -300; }); set_plane(s.spec, 1, [](int) { return -150; }, [](int r) { return r < 100 ? 0 : 1; });
    for (int i = 0; i < 3; ++i) s.exact[i] = s.predicted[i] = "LIMIT:condition-dependent";
    v.push_back(s);
  }
  {   // different answers for the same plane coordinate under different raster conditions (a water line in the middle of a cell row)
    Scenario s = make("raster-condition", "answer region changes inside a plane cell row");
    set_plane(s.spec, 0, [](int) { return -300; }); set_plane(s.spec, 1, [](int) { return -150; });
    for (int r = 100; r < H; ++r) s.spec.region[0][r] = 1;
    for (int i = 0; i < 3; ++i) s.exact[i] = s.predicted[i] = "LIMIT:condition-dependent";
    v.push_back(s);
  }
  {   // the same condition boundary on a cell row boundary: every plane coordinate keeps one answer
    Scenario s = make("raster-condition-cell-row", "answer region changes on a plane cell row boundary");
    set_plane(s.spec, 0, [](int) { return -300; }); set_plane(s.spec, 1, [](int) { return -150; });
    for (int r = 104; r < H; ++r) s.spec.region[0][r] = 1;
    v.push_back(s);
  }
  {   // level edges: the world clamps, the picture sits at the first and the last camera position
    Scenario s = make("level-start", "camera at world column 0");
    set_plane(s.spec, 0, [](int) { return 0; }); set_plane(s.spec, 1, [](int) { return 0; }); s.spec.clamp_w = 160;
    v.push_back(s);
    Scenario e = make("level-end", "camera at the last world column");
    set_plane(e.spec, 0, [](int) { return -960; }); set_plane(e.spec, 1, [](int) { return -480; }); e.spec.clamp_w = 160;
    v.push_back(e);
  }
  {   // a camera that crosses the 10-bit scroll wrap between the frame the callback saw and the frame that is produced: the plane coordinates jump by 128 cells
    Scenario s = make("wrap-crossing", "hscroll wraps from -512 to 511 between N and N + 2");
    set_plane(s.spec, 0, [](int) { return -514; }, nullptr, [](int) { return -4; }); set_plane(s.spec, 1, [](int) { return -514; }, nullptr, [](int) { return -4; });
    for (int i = 0; i < 3; ++i) s.predicted[i] = "LIMIT:window";
    v.push_back(s);
  }
  {   // a fast camera: the prediction only needs the padded margin
    Scenario s = make("fast-camera", "12 px per frame, padded by 4 cells");
    set_plane(s.spec, 0, [](int) { return -300; }, nullptr, [](int) { return -12; }); set_plane(s.spec, 1, [](int r) { return -(100 + (r / 56) * 40); }, nullptr, [](int r) { return -(6 + (r / 56)); });
    v.push_back(s);
  }
  return v;
}

struct Tally { unsigned cases = 0, pass = 0, limit = 0; };

void run_matrix(const Scenario &sc, const Aspect &asp, int ai, Timing timing, Tally &t) {
  const int side = asp.side;
  const std::string &want = timing == Timing::Exact ? sc.exact[ai] : sc.predicted[ai];
  const int placements[] = {0, side / 2, side, side + side / 2, 2 * side};   // left-aligned, intermediate, centred, intermediate, right-aligned (picture at left_px)
  for (const int left : placements) {
    const Verdict v = run_case(sc.spec, side, left, timing);
    ++t.cases;
    char what[256];
    std::snprintf(what, sizeof what, "%s %s left_px=%d %s: %s (rows differing %u: window %u, fallback %u, condition %u, unexplained %u; windows %u+%u, needed %u+%u)", sc.spec.id.c_str(), asp.name, left,
                  timing == Timing::Exact ? "exact" : "predicted", v.scenario.c_str(), v.rows_differ, v.rows_window, v.rows_fallback, v.rows_condition, v.rows_unexplained, v.windows[0], v.windows[1], v.needed[0], v.needed[1]);
    std::printf("PARITY %s\n", what);
    // placement-independence of the verdict except where the placement itself decides the spans (an all-left or all-right placement asks only one side)
    const bool one_sided = left == 0 || left == 2 * side;
    check(v.scenario == want || (one_sided && v.scenario == "PASS" && want != "PASS"), std::string("expected ") + want + ": " + what);
    check(v.rows_unexplained == 0, std::string("every difference is attributed: ") + what);
    if (v.scenario == "PASS") { ++t.pass; check(v.digest1 == v.digest2 && v.rows_differ == 0, std::string("PASS means identical surfaces: ") + what); }
    else { ++t.limit; check(v.rows_differ > 0 && v.digest1 != v.digest2, std::string("a claimed limit fails deterministically: ") + what); }
  }
}

void cost_report() {
  // Class-level cost of the provider-side work per produced frame: the ABI 1 provider answers every off-picture cell of both planes in its own code (one call per pixel); an ABI 2 provider fills at most
  // two bounded windows per plane (<= 4 x 4096 words, 4 x 8 KiB copied by the host) and the host answers the same pixels from them. Deterministic counts; the wall-clock share of the producer is the
  // measurement of docs/architecture/widescreen-scenery-lookup-measurement.md (the lookup itself is unchanged).
  for (const char *id : {"constant", "parallax-many", "parallax-interleaved"})
    for (const Scenario &sc : scenarios()) {
      if (sc.spec.id != id) continue;
      for (const Aspect &asp : kAspects) {
        const Verdict v = run_case(sc.spec, asp.side, asp.side, Timing::Exact);
        std::printf("PARITY cost %s %s centred: abi1 provider calls %u, abi2 window words %u (<= %u), windows %u+%u\n", id, asp.name, v.abi1_calls, v.words, 4U * segarecomp::scenery::kWordsMax, v.windows[0], v.windows[1]);
        check(v.words <= 4U * segarecomp::scenery::kWordsMax && v.abi1_calls > v.words, std::string("windows are bounded and cheaper than per-pixel provider calls: ") + id + " " + asp.name);
      }
    }
}

void placement_errors() {
  // A builder that computed its windows for another placement (a stale margin) than the one it declares is a PROVIDER_ERROR of the test and never a window limit.
  Spec s; s.id = "placement-error";
  set_plane(s, 0, [](int) { return -300; }); set_plane(s, 1, [](int) { return -150; });
  for (const Aspect &asp : kAspects) {
    const int side = asp.side;
    const Verdict stale = run_case(s, side, side, Timing::Exact, side / 2);   // declares centred, built for an intermediate placement
    const Verdict right = run_case(s, side, 2 * side, Timing::Exact, 0);      // declares right-aligned, built for left-aligned
    std::printf("PARITY placement-error %s: %s, %s\n", asp.name, stale.scenario.c_str(), right.scenario.c_str());
    check(stale.scenario == "PROVIDER_ERROR:placement" && right.scenario == "PROVIDER_ERROR:placement", std::string("a stale placement is a provider error, not a limit: ") + asp.name);
    check(stale.rows_differ > 0 && right.rows_differ > 0, std::string("the stale placement actually differs: ") + asp.name);
  }
}

void maximum_side() {
  // The widest planned side (672 px per side): the constant and the moderate parallax shapes stay expressible (one window per side, never wider than 128 cells); the widest possible scroll spread (interleaved) needs three windows.
  for (const char *id : {"constant", "parallax-few", "parallax-interleaved"}) {
    const std::string want = std::string(id) == "parallax-interleaved" ? "LIMIT:window" : "PASS";   // the widest spread of scroll values plus 1344 planned columns needs three 128-column windows
    for (const Scenario &sc : scenarios()) {
      if (sc.spec.id != id) continue;
      for (const int left : {0, 672, 1344, 336}) {
        const Verdict v = run_case(sc.spec, 672, left, Timing::Exact);
        std::printf("PARITY %s side=672 left_px=%d exact: %s (windows %u+%u, words %u)\n", id, left, v.scenario.c_str(), v.windows[0], v.windows[1], v.words);
        check(v.scenario == want || (want != "PASS" && v.scenario == "PASS" && (left == 0 || left == 1344)), std::string("672 px per side: expected ") + want + ": " + id);
      }
    }
  }
}

}  // namespace

int main() {
  Tally exact, predicted;
  const std::vector<Scenario> all = scenarios();
  for (const Scenario &sc : all)
    for (int ai = 0; ai < 3; ++ai) {
      run_matrix(sc, kAspects[ai], ai, Timing::Exact, exact);
      run_matrix(sc, kAspects[ai], ai, Timing::Predicted, predicted);
    }
  placement_errors();
  maximum_side();
  cost_report();
  std::printf("PARITY totals: exact cases %u (pass %u, limit %u), predicted cases %u (pass %u, limit %u)\n", exact.cases, exact.pass, exact.limit, predicted.cases, predicted.pass, predicted.limit);
  if (failures != 0) { std::printf("abi_parity_scenery_test: %d failure(s)\n", failures); return 1; }
  std::printf("abi_parity_scenery_test: ok\n");
  return 0;
}
