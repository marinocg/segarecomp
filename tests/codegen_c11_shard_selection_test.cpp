// SEG-022-T003: deterministic single-file-vs-sharded selection for both accepted result shapes.
#include "segarecomp/codegen/c11/genesis_frontend.hpp"

#include <cstdlib>
#include <iostream>

using namespace segarecomp;

#define CHECK(c) do { if (!(c)) { std::cerr << "FAILED: " #c " line " << __LINE__ << '\n'; std::exit(1); } } while (0)

int main() {
  FrontendPartialProgram partial;
  FrontendAnalysis analysis;
  CHECK(generated_program_unit_count(partial) == 0U && generated_program_unit_count(analysis) == 0U);
  // Small program, both options: historical single file.
  CHECK(!select_sharded_generated_c(true, true, generated_program_unit_count(partial)));
  CHECK(!select_sharded_generated_c(true, true, generated_program_unit_count(analysis)));
  // Large programs of either shape: blocks + AOT entries are both counted.
  partial.accepted_prefix.static_blocks.resize(generated_c_shard_threshold - 5U);
  partial.accepted_prefix.immutable_rom_aot_entries.resize(5U);
  analysis.static_blocks.resize(generated_c_shard_threshold - 5U);
  analysis.immutable_rom_aot_entries.resize(5U);
  CHECK(generated_program_unit_count(partial) == generated_c_shard_threshold);
  CHECK(generated_program_unit_count(analysis) == generated_c_shard_threshold);
  CHECK(select_sharded_generated_c(true, true, generated_program_unit_count(partial)));
  CHECK(select_sharded_generated_c(true, true, generated_program_unit_count(analysis)));
  analysis.immutable_rom_aot_entries.resize(4U);
  CHECK(!select_sharded_generated_c(true, true, generated_program_unit_count(analysis)));  // just below
  // Alone: shard-dir forces sharding, output file forces single file, neither never shards.
  CHECK(select_sharded_generated_c(false, true, 0U));
  CHECK(!select_sharded_generated_c(true, false, 1U << 20));
  CHECK(!select_sharded_generated_c(false, false, 1U << 20));
  return 0;
}
