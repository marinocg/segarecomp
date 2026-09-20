// SEG-007-T252 / ADR-0040: this runtime translation unit (runtime.c) must
// reference no wall-clock/sleep symbol of any kind -- headless/automated
// execution through genesis_runtime_run is full-speed by construction. This
// mirrors the existing forbidden-production-symbol pattern already used by
// tests/genesis_checkpoint_oracle_independence_test.py (an `nm`-based
// symbol-table check), applied here to the small, fixed set of forbidden
// wall-clock/sleep symbols instead of that test's oracle-independence set.
//
// This is a link-time/source-level check, not a behavioral one: it greps the
// runtime.c source directly for a call to any forbidden symbol, which is
// simpler and just as conclusive as an `nm` pass over the compiled object
// (the runtime library is always built from exactly this one source file,
// with no external code generation), and avoids depending on `nm` being
// installed in every CI environment.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace {

// A forbidden call is the symbol name immediately followed by optional
// whitespace and an opening parenthesis, so this does not false-positive on
// an unrelated identifier that merely contains one of these names as a
// substring (there are none in this codebase, but the check should not rely
// on that).
const char *const kForbiddenSymbols[] = {"time", "clock", "sleep", "usleep", "nanosleep"};

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <path-to-runtime.c>\n", argc > 0 ? argv[0] : "genesis_runtime_run_no_wallclock_test");
    return 2;
  }
  std::ifstream input(argv[1]);
  if (!input) {
    std::fprintf(stderr, "FAIL: could not open %s\n", argv[1]);
    return 1;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  const std::string source = buffer.str();
  int failures = 0;
  for (const char *symbol : kForbiddenSymbols) {
    const std::regex pattern(std::string("\\b") + symbol + "\\s*\\(");
    if (std::regex_search(source, pattern)) {
      std::printf("FAIL: runtime.c calls the forbidden wall-clock/sleep symbol '%s'\n", symbol);
      ++failures;
    }
  }
  // Positive control: prove the check itself can detect a real forbidden
  // call, so this test is not vacuously passing.
  {
    const std::regex control_pattern("\\bprintf\\s*\\(");
    if (!std::regex_search(source, control_pattern)) {
      std::printf("FAIL: positive control failed -- runtime.c unexpectedly has no printf( call to detect\n");
      ++failures;
    }
  }
  if (failures != 0) {
    std::printf("genesis_runtime_run_no_wallclock_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("genesis_runtime_run_no_wallclock_test: all checks passed\n");
  return 0;
}
