// SEG-022-T003: deterministic generated-C translation-unit sharder (synthetic; no commercial data).
#include "segarecomp/codegen/c11/translation_units.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace segarecomp;
namespace fs = std::filesystem;

#define CHECK(c) do { if (!(c)) { std::cerr << "FAILED: " #c " line " << __LINE__ << '\n'; std::exit(1); } } while (0)

static std::string slurp(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

static const std::vector<TranslationUnitFamily> families{{"fn", 4U, 4U}, {"meta", 1U, 4U}};

// One synthetic program: a header block, 40 functions spread over key pages, glue, and a meta unit.
static std::string build(const fs::path &dir, bool reverse_glue_free = false) {
  (void)reverse_glue_free;
  TranslationUnitSharder sharder{dir, "gen", families};
  auto &out = sharder.stream();
  CHECK(sharding_active(out));
  shard_begin_header(out);
  out << "#include <stdint.h>\n";
  shard_end_header(out);
  out << "static const int glue_value = 7;\n";
  for (std::uint64_t key = 0; key < 40U; ++key) {
    const std::string name = "fn_" + std::to_string(key);
    ShardUnitScope unit(out, "fn", key * 16U, "int " + name + "(void)");
    out << "static int " << name << "(void) { return " << key << "; }\n";
  }
  shard_begin_unit(out, "meta", 0U, "int meta_fn(void)");
  out << "int meta_fn(void) { return 1; }\n";  // already external: nothing to strip
  shard_end_unit(out);
  out << "int main(void) { return glue_value; }\n";
  CHECK(out.good());
  CHECK(sharder.finish().empty());
  CHECK(sharder.translation_unit_count() == 1U + 4U + 1U);  // main + 4 fn shards + meta
  CHECK(sharder.max_translation_unit_count() == 1U + 4U + 1U);
  std::string all;
  for (const auto &entry : fs::directory_iterator(dir)) (void)entry;
  std::ifstream manifest(dir / "gen.units");
  for (std::string line; std::getline(manifest, line);) all += line + "\n" + slurp(dir / line);
  return all + "HEADER\n" + slurp(dir / "gen.h");
}

int main() {
  const auto base = fs::temp_directory_path() / "segarecomp_tu_test";
  fs::remove_all(base);
  const auto a = build(base / "a");
  const auto b = build(base / "b");
  CHECK(a == b);  // byte-identical across runs and directories: pure function of (family, key)

  // Bounded, exact layout: main first, then sorted.
  std::ifstream manifest(base / "a" / "gen.units");
  std::vector<std::string> names;
  for (std::string line; std::getline(manifest, line);) names.push_back(line);
  CHECK(names.size() == 6U);
  CHECK(names[0] == "gen_main.c");
  CHECK(std::is_sorted(names.begin() + 1, names.end()));

  // Shard rule (key >> 4) % 4: keys 0,16,32,... land in shards by page; a function is whole in one TU.
  const auto shard0 = slurp(base / "a" / "gen_fn_00.c");
  CHECK(shard0.find("int fn_0(void) { return 0; }") != std::string::npos);
  CHECK(shard0.find("static int fn_0") == std::string::npos);  // linkage made external
  CHECK(shard0.find("int fn_4(void) { return 4; }") != std::string::npos);  // page 4 % 4 == 0
  CHECK(shard0.find("fn_1(void)") == std::string::npos);
  CHECK(shard0.starts_with("#define _POSIX_C_SOURCE 200809L\n#include \"gen.h\"\n"));

  // Header holds the header block and one declaration per unit; glue stays in the main TU only.
  const auto header = slurp(base / "a" / "gen.h");
  CHECK(header.find("#include <stdint.h>\n") != std::string::npos);
  CHECK(header.find("int fn_39(void);\n") != std::string::npos);
  CHECK(header.find("int meta_fn(void);\n") != std::string::npos);
  CHECK(header.find("glue_value") == std::string::npos);
  CHECK(slurp(base / "a" / "gen_main.c").find("glue_value") != std::string::npos);

  // Failure paths leave nothing behind and never publish a manifest.
  {
    TranslationUnitSharder sharder{base / "bad", "gen", families};
    shard_begin_unit(sharder.stream(), "nope", 0U, "int f(void)");
    CHECK(!sharder.stream().good());
    CHECK(!sharder.finish().empty());
    CHECK(!fs::exists(base / "bad" / "gen.units"));
    for (const auto &entry : fs::directory_iterator(base / "bad")) { std::cerr << entry.path() << '\n'; CHECK(false); }
  }
  {
    TranslationUnitSharder sharder{base / "rej", "gen", families};
    shard_begin_unit(sharder.stream(), "fn", 0U, "int f(void)");
    sharder.stream() << "/* translation rejected: x */\n";
    shard_end_unit(sharder.stream());
    CHECK(!sharder.finish().empty());
    CHECK(!fs::exists(base / "rej" / "gen.units"));
  }
  {
    // Abandoned (never finished) sharder removes its partial files.
    { TranslationUnitSharder sharder{base / "gone", "gen", families}; sharder.stream() << "int x;\n"; }
    for (const auto &entry : fs::directory_iterator(base / "gone")) { std::cerr << entry.path() << '\n'; CHECK(false); }
  }
  // A plain stream is not a sharder stream: every helper is a no-op.
  std::ostringstream plain;
  CHECK(!sharding_active(plain));
  { ShardUnitScope unit(plain, "fn", 0U, "int f(void)"); plain << "text"; }
  shard_declare(plain, "int g(void)");
  CHECK(plain.str() == "text");
  fs::remove_all(base);
  return 0;
}
