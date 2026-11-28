// SEG-148-T003 (ADR 0328): the code-eligibility rules over hand-written evidence, for the cases a synthetic build does not produce (a contradicted boot contract, sub and main extents sharing disc
// bytes, one disc range admitted at two addresses). The end-to-end route over real build evidence is tests/mod_disc_code_test.py.
#include "segarecomp/mod_disc.hpp"

#include <cstdio>
#include <string>

namespace {
using namespace segarecomp::mod;
int failures = 0;

void expect(const DiscSpan &span, const DiscEvidence &evidence, const std::string &want, const char *label) {
  DiscPlan plan;
  plan.ok = true;
  plan.spans = {span};
  const auto r = check_disc_eligibility(plan, segarecomp::DiscImage{}, evidence);
  const std::string got = r ? r->code : std::string("ok");
  if (got != want) {
    std::printf("FAIL %s: want %s, got %s\n", label, want.c_str(), got.c_str());
    ++failures;
  }
}
}  // namespace

int main() {
  const DiscSpan code{0x400, 4, 0xFF0200, true, "patch"}, data{0x400, 4, 0, false, "edit"};
  const DiscProof boot{"main", "boot", 0x200, 0x1000, 0xFF0000}, contradicted{"main", "boot_contradicted", 0x200, 0x1000, 0xFF0000};
  const DiscProof sub{"sub", "static", 0x402, 2, 0x6000}, mirror{"main", "resolved", 0x400, 0x100, 0x200000};
  expect(code, {{boot}, {}}, "ok", "a proven main extent");
  expect(code, {{contradicted}, {}}, "unsupported_code_class:not_disc_proven", "a contradicted boot contract authorizes nothing");
  expect(data, {{contradicted}, {}}, "unsupported_code_class:undeclared_code", "a contradicted boot image still refuses an undeclared write");
  expect(code, {{contradicted, mirror}, {}}, "unsupported_code_class:not_disc_proven", "another mapping of the bytes does not prove this address");
  expect(DiscSpan{0x400, 4, 0x200000, true, "patch"}, {{boot, mirror}, {}}, "ok", "one disc range admitted at two addresses: the declared one is proven");
  expect(code, {{boot, sub}, {}}, "unsupported_code_class:sub_m68k", "a partial overlap with sub code wins over a main proof");
  expect(data, {{sub}, {}}, "unsupported_code_class:sub_m68k", "a data write over sub code");
  expect(data, {{}, {}}, "ok", "a data write over nothing proven");
  expect(DiscSpan{0x400, 4, 0xFFA000, true, "patch"}, {{boot}, {{"main", "produced", 0xFFA000, 64}}}, "unsupported_code_class:dynamic", "an address only a produced image reaches");
  expect(DiscSpan{0x400, 4, 0xFFA000, true, "patch"}, {{boot}, {{"sub", "produced", 0xFFA000, 64}}}, "unsupported_code_class:not_disc_proven", "a sub dynamic image says nothing about main");
  expect(DiscSpan{0x11FE, 4, 0xFF0FFE, true, "patch"}, {{boot}, {}}, "unsupported_code_class:not_disc_proven", "a patch that leaves the proven extent");
  std::printf("%d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
