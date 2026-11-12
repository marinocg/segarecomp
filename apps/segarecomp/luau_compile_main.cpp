// `segarecomp-luau-compile <package.segapatch> <out-dir>`: the bounded child of the trusted Luau compile step (SEG-137-T003, ADR 0294, Luau policy section 2.4).
// The CLI runs it through run_process, which kills it after the policy's 5 seconds. THIS file enforces the 256 MiB memory ceiling: it replaces the global C++ allocation functions with metered ones that count the
// live bytes requested through operator new/new[] (the compiler's AST arena, strings and vectors) and end the process with kExitMemory the moment a request would pass the ceiling. This is a bound on C++
// heap requests, identical on every platform; it does not meter direct malloc (only the compiler's single output buffer) or the stack. Nothing else lives in this process.
// Result files in <out-dir>: `compiled.bin` (CompiledMod::serialize) and `result.txt` (`ok <peak-bytes>` or `reject <class> <message>`).
// Test seams (can only LOWER the policy): SEGARECOMP_LUAU_COMPILE_MEMORY_BYTES; SEGARECOMP_LUAU_COMPILE_STALL_MS delays the start so the parent's time limit can be exercised.
#include "segarecomp/luau_host.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <thread>

namespace {

constexpr int kExitReject = 1;
constexpr int kExitMemory = 87;
constexpr std::size_t kHeader = 16;   // keeps 16-byte alignment and holds the request size

std::atomic<std::uint64_t> g_live{0}, g_peak{0};
std::atomic<std::uint64_t> g_limit{segarecomp::luau::kCompileMemoryBytesMax};

void *metered_alloc(std::size_t n) {
  const std::uint64_t limit = g_limit.load(std::memory_order_relaxed);
  const std::uint64_t live = g_live.fetch_add(n, std::memory_order_relaxed) + n;
  if (n > limit || live > limit) std::_Exit(kExitMemory);
  std::uint64_t peak = g_peak.load(std::memory_order_relaxed);
  while (live > peak && !g_peak.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {}
  void *block = std::malloc(n + kHeader);
  if (block == nullptr) std::_Exit(kExitMemory);
  std::memcpy(block, &n, sizeof n);
  return static_cast<char *>(block) + kHeader;
}

void metered_free(void *p) noexcept {
  if (p == nullptr) return;
  void *block = static_cast<char *>(p) - kHeader;
  std::size_t n = 0;
  std::memcpy(&n, block, sizeof n);
  g_live.fetch_sub(n, std::memory_order_relaxed);
  std::free(block);
}

}  // namespace

void *operator new(std::size_t n) { return metered_alloc(n); }
void *operator new[](std::size_t n) { return metered_alloc(n); }
void *operator new(std::size_t n, const std::nothrow_t &) noexcept { return metered_alloc(n); }
void *operator new[](std::size_t n, const std::nothrow_t &) noexcept { return metered_alloc(n); }
void operator delete(void *p) noexcept { metered_free(p); }
void operator delete[](void *p) noexcept { metered_free(p); }
void operator delete(void *p, std::size_t) noexcept { metered_free(p); }
void operator delete[](void *p, std::size_t) noexcept { metered_free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { metered_free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { metered_free(p); }

namespace {

std::uint64_t env_number(const char *name) {
  const char *v = std::getenv(name);
  if (v == nullptr || *v == '\0') return 0;
  std::uint64_t n = 0;
  for (; *v != '\0'; ++v) {
    if (*v < '0' || *v > '9' || n > (UINT64_MAX - 9) / 10) return 0;
    n = n * 10 + static_cast<std::uint64_t>(*v - '0');
  }
  return n;
}

int write_result(const std::filesystem::path &dir, const std::string &text) {
  std::ofstream f(dir / "result.txt", std::ios::binary | std::ios::trunc);
  f << text << '\n';
  return f ? 0 : 2;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  const std::filesystem::path package = argv[1], out = argv[2];
  const std::uint64_t lower = env_number("SEGARECOMP_LUAU_COMPILE_MEMORY_BYTES");
  if (lower != 0 && lower < g_limit.load()) g_limit.store(lower);
  if (const std::uint64_t stall = env_number("SEGARECOMP_LUAU_COMPILE_STALL_MS"); stall != 0) std::this_thread::sleep_for(std::chrono::milliseconds(stall));

  namespace luau = segarecomp::luau;
  const segarecomp::mod::Package p = segarecomp::mod::read_package_file(package);
  if (!p.ok) return write_result(out, "reject " + p.rejection.code + " " + p.rejection.message) == 0 ? kExitReject : 2;
  const luau::CompileResult c = luau::compile_mod(p);
  if (!c.ok) {
    std::string message = c.rejection.message;
    for (char &ch : message) if (ch == '\n' || ch == '\r') ch = ' ';
    return write_result(out, "reject " + c.rejection.code + " " + message) == 0 ? kExitReject : 2;
  }
  if (!c.compiled.empty()) {
    const std::vector<std::uint8_t> bytes = c.compiled.serialize();
    std::ofstream f(out / "compiled.bin", std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f) return 2;
  }
  return write_result(out, "ok " + std::to_string(g_peak.load())) == 0 ? 0 : 2;
}
