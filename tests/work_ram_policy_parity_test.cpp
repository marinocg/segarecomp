#include "segarecomp/machine/genesis/address_space.hpp"

#include "runtime.h"

#include <cassert>
#include <cstdint>

namespace {

void assert_runtime_result(GenesisRuntime &runtime, std::uint32_t address,
                           GenesisAccessWidth width, bool expected) {
  GenesisRuntimeStop stop{};
  std::uint32_t value = 0U;
  const auto result = genesis_route_access(&runtime, address, width,
                                           GENESIS_ACCESS_READ, &value, &stop);
  assert((result == GENESIS_ACCESS_OK) == expected);
  if (!expected) assert(stop.stop_class == GENESIS_STOP_UNSUPPORTED_MEMORY_REGION);
}

}  // namespace

int main() {
  using namespace segarecomp;

  constexpr std::uint32_t begin = SEGARECOMP_GENESIS_WORK_RAM_BEGIN;
  constexpr std::uint32_t end = SEGARECOMP_GENESIS_WORK_RAM_END;
  static_assert(m68k_startup_ram_begin == begin);
  static_assert(m68k_startup_ram_end == end);

  assert(!m68k_startup_ram_range_in_range(begin, 0U));
  assert(!segarecomp_genesis_work_ram_contains(begin, 0U));
  assert(m68k_startup_ram_range_in_range(begin, 1U));
  assert(m68k_startup_ram_range_in_range(begin, 2U));
  assert(m68k_startup_ram_range_in_range(begin, 4U));
  assert(m68k_startup_ram_range_in_range(end - 1U, 1U));
  assert(m68k_startup_ram_range_in_range(end - 2U, 2U));
  assert(m68k_startup_ram_range_in_range(end - 4U, 4U));
  assert(!m68k_startup_ram_range_in_range(begin - 1U, 1U));
  assert(!m68k_startup_ram_range_in_range(end - 1U, 2U));
  assert(!m68k_startup_ram_range_in_range(end - 3U, 4U));

  GenesisRuntime runtime{};
  assert_runtime_result(runtime, begin, static_cast<GenesisAccessWidth>(0), false);
  assert_runtime_result(runtime, begin, GENESIS_ACCESS_BYTE, true);
  assert_runtime_result(runtime, begin, GENESIS_ACCESS_WORD, true);
  assert_runtime_result(runtime, begin, GENESIS_ACCESS_LONG, true);
  assert_runtime_result(runtime, end - 1U, GENESIS_ACCESS_BYTE, true);
  assert_runtime_result(runtime, end - 2U, GENESIS_ACCESS_WORD, true);
  assert_runtime_result(runtime, end - 4U, GENESIS_ACCESS_LONG, true);
  assert_runtime_result(runtime, begin - 1U, GENESIS_ACCESS_BYTE, false);
  assert_runtime_result(runtime, end - 1U, GENESIS_ACCESS_WORD, false);
  assert_runtime_result(runtime, end - 3U, GENESIS_ACCESS_LONG, false);
}
