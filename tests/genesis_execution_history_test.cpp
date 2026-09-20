// SEG-020-T003: opt-in bounded execution history at the retire seam.
// Project-authored synthetic values only.
#include "runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
int failures = 0;
void check(bool ok, const char *what) {
  if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

std::string dump(const GenesisRuntime &runtime) {
  char *buffer = nullptr;
  size_t size = 0;
  FILE *file = open_memstream(&buffer, &size);
  const int rc = genesis_write_ephemeral_execution_history(file, &runtime);
  std::fclose(file);
  std::string out(buffer, size);
  std::free(buffer);
  check(rc == 0, "writer succeeds");
  return out;
}

struct Outcome { uint32_t pc; uint32_t kind; uint32_t next; };

Outcome drive(GenesisRuntime *runtime, uint32_t count) {
  GenesisControlTransfer last{};
  for (uint32_t i = 0; i < count; ++i)
    last = genesis_runtime_retire_m68k_instruction(runtime, 4U + i % 3U, 0x1000U + i * 2U);
  return {runtime->pc, static_cast<uint32_t>(last.kind), last.next_pc};
}
}  // namespace

int main() {
  GenesisExecutionHistory history{};
  GenesisRuntime a{};
  a.execution_history = &history;
  const uint32_t total = GENESIS_EXECUTION_HISTORY_CAPACITY * 2U + 5U;
  drive(&a, total);
  check(history.total_recorded == total, "total counts overflowed events");
  const std::string text = dump(a);
  size_t events = 0;
  for (size_t pos = 0; (pos = text.find("\"seq\"", pos)) != std::string::npos; ++pos) ++events;
  check(events == GENESIS_EXECUTION_HISTORY_CAPACITY, "dump is bounded by capacity");
  const uint32_t first = total - GENESIS_EXECUTION_HISTORY_CAPACITY;
  check(text.rfind("[{\"seq\":" + std::to_string(first) + ",", 0) == 0, "oldest retained is total-capacity");
  check(text.find("\"seq\":" + std::to_string(total - 1U) + ",") != std::string::npos, "newest retained");

  GenesisExecutionHistory history2{};
  GenesisRuntime b{};
  b.execution_history = &history2;
  drive(&b, total);
  check(dump(b) == text, "identical runs dump identically");

  GenesisRuntime off{};
  GenesisRuntime on{};
  GenesisExecutionHistory history3{};
  on.execution_history = &history3;
  const Outcome o1 = drive(&off, 50U);
  const Outcome o2 = drive(&on, 50U);
  check(o1.pc == o2.pc && o1.kind == o2.kind && o1.next == o2.next, "results unchanged by history");
  check(dump(off) == "[]\n", "disabled history dumps empty array");
  GenesisRuntime empty{};
  GenesisExecutionHistory none{};
  empty.execution_history = &none;
  check(dump(empty) == "[]\n", "empty enabled history dumps empty array");

  if (failures != 0) return 1;
  std::puts("genesis_execution_history_tests passed");
  return 0;
}
