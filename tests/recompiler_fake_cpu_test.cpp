// SEG-018-T003: a tiny project-authored fake CPU consuming only the generic
// recompiler contracts. This target links segarecomp::recompiler alone; it must
// compile without any MC68000 (or other real CPU) header.
#include "segarecomp/recompiler/frontend.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
struct FakeFrontier {
  std::uint32_t address;
  int kind;
  bool operator==(const FakeFrontier &o) const { return address == o.address && kind == o.kind; }
};
struct FakeProgram { std::vector<std::uint32_t> addresses; };
} // namespace

int main() {
  using namespace segarecomp;
  RecompilerPartialProgram<FakeProgram, FakeFrontier> partial{{{0x10, 0x20}}, {{0x40, 1}, {0x30, 2}, {0x40, 1}}};
  auto key = [](const FakeFrontier &f) { return std::pair{f.address, f.kind}; };

  const auto bounded = recompiler_sort_dedup_and_bound_frontiers(partial.frontiers, 2, key);
  if (!bounded || bounded->size() != 2 || (*bounded)[0].address != 0x30 || (*bounded)[1].address != 0x40) return 1;
  if (recompiler_sort_dedup_and_bound_frontiers(partial.frontiers, 1, key)) return 2;

  const auto projection = recompiler_sort_dedup_and_project_frontiers(partial.frontiers, 1, key);
  if (projection.complete.size() != 2 || projection.bounded.size() != 1 || projection.residual_count != 1) return 3;
  if (projection.bounded[0].address != 0x30) return 4;
  std::puts("recompiler fake CPU test: ok");
  return 0;
}
