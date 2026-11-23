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
    if (timing == Timing::Exact) check((v.uncovered == 0) == v.feasible, std::string("a feasible builder represents every requested cell, an infeasible one does not: ") + what);
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

// ---- the interval cover (the column-slicing fallback of the builder), tested against an independent checker ----------------------------------------------------------------------------------------
using Iv = std::pair<long, long>;
// Independent of cover_columns: does the union of `windows` (each exactly `cap` wide) contain every integer of every interval, are the windows within the cap and is there no window beyond the need?
bool covers(const std::vector<Iv> &intervals, const std::vector<Iv> &windows, long cap) {
  for (const Iv &w : windows) if (w.second - w.first + 1 != cap) return false;
  for (const Iv &iv : intervals)
    for (long c = iv.first; c <= iv.second; ++c) {
      bool in = false;
      for (const Iv &w : windows) in = in || (c >= w.first && c <= w.second);
      if (!in) return false;
    }
  return true;
}
// The defective algorithm the review found (one slice per input interval, however wide the interval is), kept only to prove the checker and the regression detect it.
std::vector<Iv> faulty_cover_columns(std::vector<Iv> intervals, long cap) {
  std::vector<Iv> out;
  std::sort(intervals.begin(), intervals.end());
  long covered_to = 0; bool have = false;
  for (const Iv &p : intervals) {
    if (have && p.second <= covered_to) continue;
    const long start = (have && p.first <= covered_to) ? covered_to + 1 : p.first;
    out.push_back({start, start + cap - 1});
    covered_to = start + cap - 1; have = true;
  }
  return out;
}
void cover_tests() {
  const long cap = 128;
  struct Case { const char *name; std::vector<Iv> iv; size_t want; } cases[] = {
      {"shorter than the cap", {{10, 100}}, 1},
      {"exactly the cap", {{-64, 63}}, 1},
      {"one over the cap needs two windows", {{0, 128}}, 2},
      {"169 columns (the 672 px right extension at a constant scroll) need two windows", {{43, 211}}, 2},
      {"three windows", {{0, 300}}, 3},
      {"exactly two caps", {{5, 260}}, 2},
      {"overlapping intervals merge", {{0, 100}, {50, 150}, {120, 200}}, 2},
      {"adjacent intervals merge", {{0, 63}, {64, 127}}, 1},
      {"disjoint intervals", {{0, 10}, {1000, 1010}, {-500, -490}}, 3},
      {"disjoint intervals, one wide", {{0, 10}, {1000, 1200}}, 3},
      {"contained interval adds nothing", {{0, 200}, {20, 30}}, 2},
      {"no interval", {}, 0},
  };
  for (const Case &c : cases) {
    const std::vector<Iv> out = cover_columns(c.iv, cap);
    check(out.size() == c.want, std::string("minimum number of windows: ") + c.name);
    check(covers(c.iv, out, cap), std::string("every column of every interval is covered: ") + c.name);
    for (size_t i = 1; i < out.size(); ++i) check(out[i].first > out[i - 1].second, std::string("windows do not overlap: ") + c.name);
  }
  check(cover_columns({{0, 10}}, 0).empty(), "cap 0 (the plane has too many cell rows for any window) yields no window");
  // the defect: one interval of 169 columns got a single window
  const std::vector<Iv> wide = {{43, 211}};
  check(!covers(wide, faulty_cover_columns(wide, cap), cap), "the checker detects the defective one-slice-per-interval cover");
  check(covers(wide, cover_columns(wide, cap), cap), "the corrected cover represents the 169-column interval");
}

void wide_placement_tests() {
  // 320 px picture, planned side 672 (1344 extra columns), constant hscroll -300: the one-sided right extension is 168 plane cells, the left-only one the same; both need two windows and, with only two
  // windows allowed, every requested cell must be represented (a passing case means no cell is missing, not only that pixels agree).
  Spec s; s.id = "wide-constant";
  set_plane(s, 0, [](int) { return -300; }); set_plane(s, 1, [](int) { return -300; });
  for (const int left : {0, 1344, 672, 336}) {
    const Verdict v = run_case(s, 672, left, Timing::Exact);
    std::printf("PARITY wide-constant side=672 left_px=%d: %s (uncovered %u, windows %u+%u, needed %u+%u)\n", left, v.scenario.c_str(), v.uncovered, v.windows[0], v.windows[1], v.needed[0], v.needed[1]);
    check(v.scenario == "PASS" && v.uncovered == 0 && v.feasible, std::string("672 px per side, constant scroll, every requested cell represented: left_px=") + std::to_string(left));
    check(v.needed[0] >= 1 && v.needed[0] <= 2 && v.needed[1] >= 1 && v.needed[1] <= 2, "needed window count is the true minimum (1 or 2)");
    if (left == 0 || left == 1344) check(v.needed[0] == 2 && v.needed[1] == 2, "a 168-column one-sided extension needs two windows");
  }
  // more windows needed than may be submitted: the count is still exact and the shortfall is reported as a limit, never as a PASS
  Spec t; t.id = "wide-three";
  set_plane(t, 0, [](int) { return -300; }); set_plane(t, 1, [](int r) { return ((r / 8) & 1) != 0 ? 500 : -500; });
  const Verdict v3 = run_case(t, 672, 0, Timing::Exact);
  std::printf("PARITY wide-three side=672 left_px=0: %s (uncovered %u, needed %u+%u)\n", v3.scenario.c_str(), v3.uncovered, v3.needed[0], v3.needed[1]);
  check(v3.needed[1] >= 3 && !v3.feasible && v3.uncovered > 0 && v3.scenario == "LIMIT:window", "three windows needed, two submitted: exact count, limit, cells missing");
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


// ---- SEG-152-T007: the per-row generic mask (ABI 1 own_plane lines) -------------------------------------------------------------------------------------------------------
// Every fixture is run through the ABI 1 reference (which answers a fallback line with 1 and the VDP's own frame-start word, never GENESIS_COMPAT_WORD_GENERIC) and the mask-using ABI 2 builder, whose twin answers
// through the PRODUCTION `scenery::answer` and the host-private code 3. Equality is asserted on pixels AND on compat_accepted/reason, authored, authored_rows and authored_row_count (no tolerance).
struct MaskFixture { Spec spec; const char *what; };
Spec base_spec(const char *id) { Spec s; s.id = id; set_plane(s, 0, [](int) { return -300; }); set_plane(s, 1, [](int) { return -150; }); return s; }
std::vector<MaskFixture> mask_fixtures() {
  std::vector<MaskFixture> v;
  {   // the T001 failing fixture (own_plane from line 100 on plane B; line 100 is inside plane cell row 12)
    Spec s = base_spec("fallback-mid-cell");
    for (int r = 100; r < H; ++r) s.fallback[1][r] = 1;
    v.push_back({s, "T001 fallback-mid-cell"});
  }
  { Spec s = base_spec("plane-a-only"); for (int r = 60; r < H; ++r) s.fallback[0][r] = 1; v.push_back({s, "per-row fallback on plane A only (B authored: credited)"}); }
  { Spec s = base_spec("plane-b-only"); for (int r = 60; r < 190; ++r) s.fallback[1][r] = 1; v.push_back({s, "per-row fallback on plane B only (A authored: credited)"}); }
  { Spec s = base_spec("different-masks"); for (int r = 40; r < 80; ++r) s.fallback[0][r] = 1; for (int r = 100; r < H; ++r) s.fallback[1][r] = 1; v.push_back({s, "different masks on the two planes"}); }
  { Spec s = base_spec("shared-cell"); s.fallback[1][35] = 1; v.push_back({s, "one generic row sharing a plane cell coordinate with authored rows"}); }
  { Spec s = base_spec("alternating"); for (int r = 0; r < H; r += 2) s.fallback[1][r] = 1; v.push_back({s, "alternating authored and generic rows"}); }
  { Spec s = base_spec("boundary-in-cell"); for (int r = 103; r < H; ++r) s.fallback[1][r] = 1; v.push_back({s, "fallback boundary inside an 8-pixel cell row"}); }
  { Spec s = base_spec("both-masked"); for (int r = 120; r < H; ++r) s.fallback[0][r] = s.fallback[1][r] = 1; v.push_back({s, "both planes masked on a row (credited like ABI 1, which GENERIC would lose)"}); }
  { Spec s = base_spec("all-rows-masked"); for (int p = 0; p < 2; ++p) for (int r = 0; r < H; ++r) s.fallback[p][r] = 1; v.push_back({s, "every row masked on both planes (authored stays 1)"}); }
  {   // no authored windows anywhere: plane A is left to the generic extension on every row (the ABI 1 provider returns GENERIC), plane B marks rows 100+ and leaves the others generic
    Spec s = base_spec("mask-only");
    for (int r = 0; r < H; ++r) s.generic2[0][r] = 1;
    for (int r = 0; r < H; ++r) { if (r >= 100) s.fallback[1][r] = 1; else s.generic2[1][r] = 1; }
    v.push_back({s, "mask without any window: frame served by the set bit, unmarked rows generic and uncredited"});
  }
  {   // mask on one plane, windows (authored words) on the other
    Spec s = base_spec("mask-one-plane-windows-other");
    for (int r = 0; r < H; ++r) { if (r >= 100) s.fallback[1][r] = 1; else s.generic2[1][r] = 1; }
    v.push_back({s, "plane B mask only, plane A authored windows"});
  }
  {   // a window-plane status band: plane A is left out on its rows and plane B is none (the producer's own rule): no credit, whatever the mask says
    Spec s = base_spec("window-band"); s.window_from_row = 192; for (int r = 100; r < H; ++r) s.fallback[1][r] = 1; v.push_back({s, "window-plane edge / HUD band rows keep the producer's behaviour"});
  }
  {   // a name-table write above a masked row: the frame-start word is what ABI 1 reads (cell row 15 is rows 120..127)
    Spec s = base_spec("journal-write"); for (int r = 100; r < H; ++r) s.fallback[1][r] = 1; s.journal.push_back({50, 1, 15}); v.push_back({s, "mid-frame name-table write on cells of masked rows"});
  }
  {   // a frame without a VRAM journal: authored credit withheld for every provider
    Spec s = base_spec("no-journal"); for (int r = 100; r < H; ++r) s.fallback[1][r] = 1; s.no_journal = true; v.push_back({s, "non-journaled frame"});
  }
  {   // the scroll and vscroll vary: the answer comes from the line's own scroll in both providers
    Spec s = base_spec("parallax-mask"); set_plane(s, 1, [](int r) { return -(100 + (r / 38) * 40); }); s.vs[1] = 12; for (int r = 90; r < 170; ++r) s.fallback[1][r] = 1; v.push_back({s, "parallax plane with a vertical scroll and a masked band"});
  }
  return v;
}

Options mask_options() { Options o; o.use_mask = true; return o; }

void mask_parity_matrix(Tally &t) {
  for (const MaskFixture &f : mask_fixtures())
    for (int ai = 0; ai < 3; ++ai) {
      const Aspect &asp = kAspects[ai];
      const int side = asp.side;
      for (const int left : {0, side / 2, side, side + side / 2, 2 * side}) {
        const Verdict v = run_case(f.spec, side, left, Timing::Exact, -1, mask_options());
        ++t.cases;
        char what[256];
        std::snprintf(what, sizeof what, "%s %s left_px=%d: %s (rows differing %u; authored %u/%u rows %u/%u accepted %u/%u reason %u/%u)", f.spec.id.c_str(), asp.name, left, v.scenario.c_str(), v.rows_differ, v.authored1, v.authored2,
                      v.authored_rows1, v.authored_rows2, v.accepted1, v.accepted2, v.reason1, v.reason2);
        std::printf("PARITY mask %s\n", what);
        check(v.scenario == "PASS" && v.rows_differ == 0 && v.digest1 == v.digest2, std::string("pixel-identical to ABI 1: ") + what);
        check(v.coverage_equal && v.accepted1 == v.accepted2 && v.reason1 == v.reason2 && v.authored1 == v.authored2 && v.authored_rows1 == v.authored_rows2, std::string("coverage equal to ABI 1: ") + what);
        check(v.abi1_calls == v.abi2_calls, std::string("no additional host cell queries / provider calls: ") + what);
        if (v.scenario == "PASS") ++t.pass; else ++t.limit;
      }
    }
}

void mask_accounting_cases() {
  const int side = kAspects[2].side;   // 32:9
  const auto run = [&](const Spec &s, const Options &o, int left = 238) { return run_case(s, side, left, Timing::Exact, -1, o); };
  const auto fixture = [&](const char *id) -> const Spec & { static const std::vector<MaskFixture> all = mask_fixtures(); for (const MaskFixture &f : all) if (f.spec.id == id) return f.spec; std::abort(); };
  // (1)/(2): one plane masked, the other authored: credited rows (and they are not zero)
  check(run(fixture("plane-a-only"), mask_options()).authored_rows2 > 0 && run(fixture("plane-b-only"), mask_options()).authored_rows2 > 0, "case 1/2: a row masked on one plane and authored on the other is credited");
  // (3): both planes masked: credited, equal to ABI 1; the GENERIC control loses the credit (the mask is not equivalent to ADR 0314)
  {
    Options g = mask_options(); g.mask_code = GENESIS_COMPAT_WORD_GENERIC;
    const Verdict ok = run(fixture("both-masked"), mask_options()), generic = run(fixture("both-masked"), g);
    std::printf("PARITY mask accounting both-masked: authored_rows abi1 %u, code 3 %u, GENERIC control %u\n", ok.authored_rows1, ok.authored_rows2, generic.authored_rows2);
    check(ok.authored_rows1 > 0 && ok.authored_rows1 == ok.authored_rows2, "case 3: both planes masked, the rows are credited as under ABI 1");
    check(generic.authored_rows2 < ok.authored_rows1 && !generic.coverage_equal, "case 3 negative control: answering GENERIC for masked rows loses the credit");
  }
  // (4): mask-only: the frame is served by the set bit and unmarked rows are uncredited; a mask without a set bit changes nothing (here: no plane marks => nothing served)
  {
    const Verdict v = run(fixture("mask-only"), mask_options());
    check(v.accepted2 == GENESIS_COMPAT_SCENERY && v.authored_rows2 == v.authored_rows1 && v.authored_rows2 == static_cast<unsigned>(H - 100) && v.authored2 == v.authored1 && v.coverage_equal, "case 4: mask only: marked rows credited (124 of 224), unmarked rows generic uncredited, frame served");
  }
  // all rows masked: authored stays 1
  check(run(fixture("all-rows-masked"), mask_options()).authored2 == 1, "an all-rows-masked frame is authored");
  // (5): HUD band rows are not credited
  {
    const Verdict v = run(fixture("window-band"), mask_options());
    check(v.coverage_equal && v.authored_rows2 <= 192U, "case 5: window-plane band rows earn no credit");
  }
  // (6): a non-journaled frame withholds the authored claim and the rows
  {
    const Verdict v = run(fixture("no-journal"), mask_options());
    check(v.coverage_equal && v.authored2 == 0 && v.authored_rows2 == 0, "case 6: a non-journaled frame: authored 0 and no authored rows, exactly as ABI 1");
  }
  // (7): mid-frame write: identical to ABI 1 (frame-start word); the GENERIC path (evolving VRAM) differs
  {
    Options g = mask_options(); g.mask_code = GENESIS_COMPAT_WORD_GENERIC;
    const Verdict ok = run(fixture("journal-write"), mask_options()), generic = run(fixture("journal-write"), g);
    std::printf("PARITY mask journal: code 3 rows differing %u, GENERIC control rows differing %u\n", ok.rows_differ, generic.rows_differ);
    check(ok.rows_differ == 0 && ok.digest1 == ok.digest2, "case 7: the masked cell reads the frame-start VRAM (pixel-identical to ABI 1)");
    check(generic.rows_differ > 0 && generic.digest1 != generic.digest2, "case 7 negative control: the GENERIC path reads the evolving VRAM and differs");
  }
  // negative control: clearing the mask makes the T001 failing fixture differ again (and the unmarked rows were resolved from authored words)
  {
    Options c = mask_options(); c.clear_mask = true;
    const Verdict v = run(fixture("fallback-mid-cell"), c);
    check(v.rows_differ > 0 && v.digest1 != v.digest2, "clearing the mask makes the fixture differ again");
  }
}

// The code-3 gate (explicit cases a-f, exact outcomes).
void code3_gate() {
  const int side = kAspects[1].side;
  Spec s = base_spec("gate"); for (int p = 0; p < 2; ++p) for (int r = 0; r < H; ++r) s.fallback[p][r] = 1;   // every row masked on both planes
  const auto twin_with = [&](unsigned private_words, int code, const char *table_name) {
    Scene sc; build_scene(sc, s);
    Provider prov; prov.kind = Provider::Abi2; prov.spec = &s; prov.scene = &sc; prov.declared_left = side;
    prov.private_words = static_cast<uint8_t>(private_words); prov.mask_code = code;
    uint8_t bytes[28]; std::memset(bytes, 0xFF, sizeof bytes);
    for (unsigned p = 0; p < 2; ++p) segarecomp::scenery::mask_store(prov.mask[p], bytes, 28);
    GenesisNativeCompat table = twin_table(); table.name = table_name;
    prov.table = &table;
    return produce(sc, prov, static_cast<unsigned>(side));
  };
  const Produced legacy = twin_with(0U, GENESIS_COMPAT_WORD_VDP_START, "parity.legacy-abi1");      // (a) a legacy ABI 1 provider (the flag is disabled) returning 3
  check(legacy.rc == 0 && legacy.res.compat_reason == GENESIS_COMPAT_FAULT && legacy.res.authored == 0 && legacy.res.authored_row_count == 0, "(a) a legacy provider returning 3 is an unanswered cell: FAULT, authored 0, no credit");
  const Produced adapter = twin_with(1U, GENESIS_COMPAT_WORD_VDP_START, "scheduled-mod-host");      // (b) the trusted adapter
  check(adapter.rc == 0 && adapter.res.compat_reason == GENESIS_COMPAT_OK && adapter.res.authored == 1 && adapter.res.authored_row_count == static_cast<unsigned>(H), "(b) the trusted adapter returning 3 for a masked row is a successful provider word with full credit");
  const Produced untrusted = twin_with(0U, GENESIS_COMPAT_WORD_VDP_START, "scheduled-mod-host");   // (c) the same table, the flag not set; (f) a provider NAMED like the adapter does not enable it
  check(untrusted.res.compat_reason == legacy.res.compat_reason && untrusted.res.authored == legacy.res.authored && untrusted.res.authored_row_count == legacy.res.authored_row_count && untrusted.surface == legacy.surface,
        "(c)/(f) the adapter's table without the trusted attach (and a provider named like it) is rejected exactly as (a)");
  for (const int code : {4, -1, 255}) {   // (e) other unsupported values stay fail-closed in both configurations
    for (const unsigned flag : {0U, 1U}) {
      const Produced p = twin_with(flag, code, "scheduled-mod-host");
      check(p.rc == 0 && p.res.compat_reason == GENESIS_COMPAT_FAULT && p.res.authored == 0 && p.res.authored_row_count == 0, "(e) unsupported value " + std::to_string(code) + " is fail-closed, flag " + std::to_string(flag));
    }
  }
  {   // (d) values 1, 2 and 0: identical with the flag off and on (the producer change is invisible to them)
    Spec t = base_spec("gate-d"); for (int r = 100; r < H; ++r) t.fallback[1][r] = 1; for (int r = 0; r < 40; ++r) t.generic2[0][r] = 1;
    for (const unsigned flag : {0U, 1U}) (void)flag;
    Scene sc; build_scene(sc, t);
    Provider a, b; a.kind = b.kind = Provider::Abi1; a.spec = b.spec = &t; a.scene = b.scene = &sc; a.declared_left = b.declared_left = side; a.private_words = 0U; b.private_words = 1U;   // the reference answers 1 and 2
    const Produced pa = produce(sc, a, static_cast<unsigned>(side)), pb = produce(sc, b, static_cast<unsigned>(side));
    check(pa.surface == pb.surface && coverage_equal(pa, pb), "(d) answers 1 and 2 are identical with the private-word flag off and on");
    Provider w0, w1; w0.kind = w1.kind = Provider::Abi2; w0.spec = w1.spec = &t; w0.scene = w1.scene = &sc; w0.declared_left = w1.declared_left = side; w0.private_words = 0U; w1.private_words = 1U;   // no window: 2; (0 is exercised below)
    const Produced qa = produce(sc, w0, static_cast<unsigned>(side)), qb = produce(sc, w1, static_cast<unsigned>(side));
    check(qa.surface == qb.surface && coverage_equal(qa, qb), "(d) answer 2 (a plane without windows) is identical with the flag off and on");
    w0.windows[0][0].present = w1.windows[0][0].present = true; w0.windows[0][0].cols = w1.windows[0][0].cols = 1; w0.windows[0][0].rows = w1.windows[0][0].rows = 1; w0.windows[0][0].words.assign(1, 0); w1.windows[0][0].words.assign(1, 0);
    const Produced ra = produce(sc, w0, static_cast<unsigned>(side)), rb = produce(sc, w1, static_cast<unsigned>(side));   // cells outside the one-cell window: answer 0
    check(ra.res.compat_reason == GENESIS_COMPAT_FAULT && ra.surface == rb.surface && coverage_equal(ra, rb), "(d) answer 0 (a cell outside every window) fails identically with the flag off and on");
  }
  {   // (f) the flag defaults to zero in an initialiser and the adapter never derives it from a provider table
    GenesisFrameExtension ext{};
    check(ext.compat_private_words == 0U, "(f) compat_private_words is zero-initialised");
  }
}

void mask_cost_line() {
  // The stated bounds at 32:9 (the planned side of the widest standard aspect), centred: adapter state 4 x 56 bytes, copy 2 x 56 bytes, one bit test per scenery_word call, no provider code per pixel.
  const Spec s = mask_fixtures()[0].spec;
  const int side = kAspects[2].side;
  const Verdict m = run_case(s, side, side, Timing::Exact, -1, mask_options());
  Options plain; const Verdict w = run_case(s, side, side, Timing::Exact, -1, plain);
  const unsigned state_bytes = 4U * segarecomp::scenery::kMaskBytesMax, copy_bytes = 2U * segarecomp::scenery::kMaskBytesMax;
  std::printf("PARITY cost mask 32:9 centred: adapter state %u bytes (staged + visible, both planes), copy %u bytes per frame, %u bit tests (one per scenery_word call, none calls provider code), ABI 1 provider calls %u, words %u (windowless rows excluded; %u without the mask)\n",
              state_bytes, copy_bytes, m.abi2_calls, m.abi1_calls, m.words, w.words);
  check(state_bytes == 224U && copy_bytes == 112U && m.abi2_calls == m.abi1_calls && m.abi2_calls == w.abi2_calls, "the mask adds no host queries: the lookup count equals the unmasked twin's");
}

void mask_viewport_row_index() {
  // The mask is indexed by produced SCREEN row, not by the world cell row: a vertical scroll that moves plane cell rows must not change which rows are generic; moving the native picture (left_px) does not either.
  Spec s = base_spec("row-index"); s.vs[1] = 37; for (int r = 50; r < 58; ++r) s.fallback[1][r] = 1;
  const int side = kAspects[2].side;
  Verdict first;
  bool have = false;
  for (const int left : {0, side / 2, side, side + side / 2, 2 * side}) {
    const Verdict v = run_case(s, side, left, Timing::Exact, -1, mask_options());
    check(v.scenario == "PASS" && v.coverage_equal, "a vertical scroll of 37 px: the mask follows screen rows, left_px=" + std::to_string(left));
    check(v.authored_rows2 > 0, "row-index: rows credited");
    if (have) check(v.authored_rows2 == first.authored_rows2, "moving the native picture does not change which rows are generic");
    first = v; have = true;
  }
  Scene sc; build_scene(sc, s);
  Spec wrongly = s;   // the same mask applied by WORLD cell row instead would be a different set of rows: prove the two sets differ
  unsigned screen_rows = 0, world_rows = 0;
  for (int r = 0; r < H; ++r) { screen_rows += s.fallback[1][r]; world_rows += (((r + 37) / 8) >= 50 / 8 && ((r + 37) / 8) <= 57 / 8) ? 1U : 0U; }
  check(screen_rows == 8U && world_rows != screen_rows, "the screen-row set and the world-cell-row set are different for a scrolled plane");
  (void)wrongly;
}

void mask_448_row_frame() {
  // A 448-row (interlace mode 2) frame: marks above and below row 224 both reach the producer's rows; a 224-row-limited host would silently lose the lower half.
  segarecomp::scenery::RowMask m;
  uint8_t bytes[56] = {};
  bytes[1] = 0x04; bytes[40] = 0x10;   // rows 10 and 324
  check(segarecomp::scenery::mask_in_range(0, bytes, 448, 56), "a 448-row mask is in range");
  segarecomp::scenery::mask_store(m, bytes, 56);
  check(segarecomp::scenery::row_generic(m, 10) && segarecomp::scenery::row_generic(m, 324) && !segarecomp::scenery::row_generic(m, 11) && !segarecomp::scenery::row_generic(m, 447) && !segarecomp::scenery::row_generic(m, 448), "marks above and below row 224 are kept; rows beyond 448 are never marked");
  uint8_t pad[1] = {0x20};
  check(!segarecomp::scenery::mask_in_range(0, pad, 5, 1) && segarecomp::scenery::mask_in_range(0, pad, 6, 1), "padding bits beyond `rows` must be zero");
  check(!segarecomp::scenery::mask_in_range(0, bytes, 449, 57) && !segarecomp::scenery::mask_in_range(0, bytes, 0, 0) && !segarecomp::scenery::mask_in_range(2, bytes, 8, 1) && !segarecomp::scenery::mask_in_range(0, nullptr, 8, 1) &&
        !segarecomp::scenery::mask_in_range(0, bytes, 16, 1) && !segarecomp::scenery::mask_in_range(0, bytes, 8, 2) && !segarecomp::scenery::mask_in_range(0, bytes, 8, 0), "out-of-range submissions are refused by the production validator");
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
  cover_tests();
  wide_placement_tests();
  placement_errors();
  maximum_side();
  cost_report();
  Tally masked;
  mask_parity_matrix(masked);
  mask_accounting_cases();
  code3_gate();
  mask_cost_line();
  mask_viewport_row_index();
  mask_448_row_frame();
  std::printf("PARITY mask totals: cases %u (pass %u, limit %u)\n", masked.cases, masked.pass, masked.limit);
  std::printf("PARITY totals: exact cases %u (pass %u, limit %u), predicted cases %u (pass %u, limit %u)\n", exact.cases, exact.pass, exact.limit, predicted.cases, predicted.pass, predicted.limit);
  if (failures != 0) { std::printf("abi_parity_scenery_test: %d failure(s)\n", failures); return 1; }
  std::printf("abi_parity_scenery_test: ok\n");
  return 0;
}
