// SEG-136-T006 (ADR 0293): the deterministic BPS writer, property-style on synthetic buffers of equal AND different sizes, through the existing strict reader. Synthetic data only.
#include "segarecomp/patch.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;
void check(bool ok, const char *what) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

struct Lcg {   // fixed generator: the test is itself deterministic on every host
  std::uint32_t s;
  std::uint32_t next() { s = s * 1664525U + 1013904223U; return s >> 8U; }
};

std::vector<std::uint8_t> random_bytes(Lcg &g, std::size_t n) {
  std::vector<std::uint8_t> v(n);
  for (auto &b : v) b = static_cast<std::uint8_t>(g.next());
  return v;
}

}  // namespace

int main() {
  using namespace segarecomp::patch;
  Lcg g{136};
  for (int trial = 0; trial < 200; ++trial) {
    const std::size_t sn = g.next() % 600, tn = g.next() % 600;
    std::vector<std::uint8_t> source = random_bytes(g, sn), target(tn);
    for (std::size_t i = 0; i < tn; ++i) target[i] = (i < sn && (g.next() % 5) != 0) ? source[i] : static_cast<std::uint8_t>(g.next());   // mostly equal with scattered edits
    const BpsResult a = build_bps(source, target), b = build_bps(source, target);
    check(a.ok && b.ok, "build ok");
    check(a.output == b.output, "two builds are identical");
    const BpsResult back = apply_bps(a.output, source);
    check(back.ok && back.output == target, "apply(build(x)) == x for equal and different sizes");
    if (a.ok && !a.output.empty()) {   // corruption anywhere is rejected by the reader
      std::vector<std::uint8_t> bad = a.output;
      bad[g.next() % bad.size()] ^= 0x01U;
      check(!apply_bps(bad, source).ok, "a corrupted patch is rejected");
      std::vector<std::uint8_t> stale = source;
      if (!stale.empty()) { stale[g.next() % stale.size()] ^= 0x01U; check(!apply_bps(a.output, stale).ok, "a stale source is rejected"); }
    }
  }
  // identical images: a single SourceRead; empty target; growth and truncation
  const std::vector<std::uint8_t> same(1000, 0x5A);
  const BpsResult id = build_bps(same, same);
  check(id.ok, "identity builds");
  check(apply_bps(id.output, same).output == same, "identity round trip");
  check(build_bps(same, {}).ok && apply_bps(build_bps(same, {}).output, same).output.empty(), "empty target");
  check(build_bps({}, same).ok && apply_bps(build_bps({}, same).output, {}).output == same, "empty source");
  // bounds: an image larger than the supported size is refused, never truncated
  const std::vector<std::uint8_t> huge(kMaxImageBytes + 1, 0);
  check(!build_bps(huge, same).ok && !build_bps(same, huge).ok, "oversized images are refused");
  if (failures == 0) std::puts("patch_bps_writer_test: ok");
  return failures == 0 ? 0 : 1;
}
