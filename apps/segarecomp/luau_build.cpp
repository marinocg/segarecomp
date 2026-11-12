// See luau_build.hpp.
#include "luau_build.hpp"

#include "process_run.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>

namespace segarecomp_cli {

namespace fs = std::filesystem;
using namespace segarecomp;

namespace {

std::string g_self;

std::string slurp(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool write_atomic(const fs::path &out, std::span<const std::uint8_t> bytes) {
  fs::path tmp = out;
  tmp += ".partial";
  std::error_code ec;
  fs::remove(tmp, ec);
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    f.flush();
    if (!f) { f.close(); fs::remove(tmp, ec); return false; }
  }
  fs::rename(tmp, out, ec);
  if (ec) { fs::remove(tmp, ec); return false; }
  return true;
}

fs::path luau_helper_path() {
#ifdef _WIN32
  const std::string name = "segarecomp-luau-compile.exe";
#else
  const std::string name = "segarecomp-luau-compile";
#endif
  fs::path self = g_self;
  if (self.has_parent_path()) return self.parent_path() / name;
  if (const char *path = std::getenv("PATH")) {   // started by name: find ourselves on PATH
    const std::string list = path;
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    for (std::size_t from = 0; from <= list.size();) {
      std::size_t to = list.find(sep, from);
      if (to == std::string::npos) to = list.size();
      const fs::path dir = list.substr(from, to - from);
      std::error_code ec;
      for (const char *candidate : {"segarecomp", "segarecomp.exe"})
        if (!dir.empty() && fs::is_regular_file(dir / candidate, ec)) return dir / name;
      from = to + 1;
    }
  }
  return fs::path(name);
}

unsigned luau_compile_seconds() {   // test seam: may only lower the policy limit
  unsigned seconds = luau::kCompileSecondsMax;
  if (const char *v = std::getenv("SEGARECOMP_LUAU_COMPILE_SECONDS")) {
    const unsigned long n = std::strtoul(v, nullptr, 10);
    if (n >= 1 && n < seconds) seconds = static_cast<unsigned>(n);
  }
  return seconds;
}

LuauBuild build_luau_impl(const mod::Package &package, std::span<const std::uint8_t> package_bytes) {
  LuauBuild b;
  const auto fail = [&b](std::string code, std::string message) { b.ok = false; b.rejection = {std::move(code), std::move(message)}; return std::move(b); };
  bool has_luau = false;
  for (const mod::Payload &p : package.manifest.payloads) has_luau = has_luau || p.kind == "luau-source";
  if (!has_luau) return b;
  std::random_device entropy;   // a private scratch folder; its name never reaches an output
  char suffix[17];
  std::snprintf(suffix, sizeof suffix, "%08x%08x", entropy(), entropy());
  std::error_code ec;
  const fs::path work = fs::temp_directory_path() / ("segarecomp-luau-" + std::string(suffix));
  fs::remove_all(work, ec);
  fs::create_directories(work, ec);
  struct Cleanup { const fs::path &dir; ~Cleanup() { std::error_code e; fs::remove_all(dir, e); } } cleanup{work};
  if (ec || !write_atomic(work / "package.segapatch", package_bytes)) return fail("E_COMPILE_FAILED", "the Luau sources could not be staged");
  const fs::path helper = luau_helper_path();
  const int rc = segarecomp_cli::run_process({helper.string(), (work / "package.segapatch").string(), work.string()}, work / "child.log", {}, luau_compile_seconds());
  if (rc == segarecomp_cli::kProcessTimedOut) return fail("E_LUAU_COMPILE_LIMIT", "compiling the mod exceeded the per-mod time limit");
  if (rc == 87) return fail("E_LUAU_COMPILE_LIMIT", "compiling the mod exceeded the per-mod memory limit");
  const std::string result = slurp(work / "result.txt");
  if (rc == 1 && result.rfind("reject ", 0) == 0) {   // `reject <class> <message>`
    const std::size_t sp = result.find(' ', 7);
    std::string message = sp == std::string::npos ? "" : result.substr(sp + 1);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) message.pop_back();
    return fail(result.substr(7, sp == std::string::npos ? std::string::npos : sp - 7), message);
  }
  const std::string stored = slurp(work / "compiled.bin");
  const std::span<const std::uint8_t> bytes{reinterpret_cast<const std::uint8_t *>(stored.data()), stored.size()};
  if (rc != 0 || result.rfind("ok ", 0) != 0) return fail("E_COMPILE_FAILED", rc < 0 ? "the Luau compile helper could not be started" : "the Luau compile helper stopped unexpectedly");
  const luau::CompileResult v = luau::verify_compiled(package, bytes, luau::stored_digest(bytes));
  if (!v.ok) return fail("E_COMPILE_FAILED", "the Luau compile helper returned output that does not verify");
  b.compiled = v.compiled;
  b.json = ",\"luau\":{\"compiler\":\"luau-" + std::string(luau::compiler_pin()) + "\",\"scripts\":" + std::to_string(v.compiled.scripts().size()) + ",\"bytecode_digest\":\"" + v.compiled.digest() + "\"}";
  return b;
}

}  // namespace

void set_self_path(const std::string &argv0) { g_self = argv0; }

LuauBuild build_luau(const mod::Package &package, std::span<const std::uint8_t> package_bytes) { return build_luau_impl(package, package_bytes); }

}  // namespace segarecomp_cli
