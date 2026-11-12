#pragma once

// Luau scripting, host side (ADR 0284 trust policy, ADR 0294 implementation; SEG-137-T001..T003). Two seams:
//
//  * The TRUSTED COMPILE STEP (luau_compile.cpp). Packages carry Luau source only. `compile_mod` turns the verified `luau-source` payloads of a package into bytecode with the pinned compiler (fixed options)
//    and binds the result to the package identity with a bytecode digest. `verify_compiled` is the only way to turn stored compiled bytes back into a `CompiledMod`: it checks the digest recorded at build
//    time, the package identity and every source digest. There is no function that accepts bytecode from a package, and `Vm::load` accepts nothing but a `CompiledMod`.
//  * The RESTRICTED VM (luau_vm.cpp). One VM per mod: whitelisted libraries, read-only globals, deterministic `math.random`, a hard memory quota, a deterministic instruction budget enforced from the
//    interrupt callback, a call-depth limit, a wall-time watchdog that is only a hang backstop (a watchdog fault marks the session non-deterministic), fault isolation and leak-checked teardown.
//
// No host API (`sega.*`) exists yet (SEG-137-T004). The VM is "restricted", never claimed "safe" (policy section 7).

#include "segarecomp/mod.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace segarecomp::luau {

// ---- pinned compiler ----
// The vendored release (third_party/luau/PIN.txt). Part of every bytecode digest, so a compiler update is a new build identity.
[[nodiscard]] std::string_view compiler_pin();
// Fixed compile options: optimization level 1, debug level 0, no type info, no coverage (policy section 2.5).
inline constexpr std::size_t kMaxBytecodeBytes = 512U << 10U;   // per mod (policy section 4)
inline constexpr unsigned kCompileSecondsMax = 5;               // per mod; exceeding it is E_LUAU_COMPILE_LIMIT
inline constexpr std::uint64_t kCompileMemoryBytesMax = 256ULL << 20U;   // per mod, live C++ heap bytes of the compile
// compile_mod itself runs IN THIS PROCESS and meters nothing. The two bounds above are enforced by the product's compile child (apps/segarecomp/luau_compile_main.cpp, run by `mod build`/`mod variant`
// with a hard kill timeout and a metered allocator); compile_mod's own elapsed-time check is a second line for in-process callers (tests), not the enforcement.

// A compile diagnostic: always `<package path>:<line>: <message>`, the line 1-based, the message the compiler's first error, bounded.
struct Diagnostic {
  std::string file;
  unsigned line = 0;
  std::string message;
  [[nodiscard]] std::string text() const;
};

struct CompiledScript {
  std::string path, source_sha256, bytecode_sha256;
  std::vector<std::uint8_t> bytecode;
};

// The compiled form of ONE package. Only compile_mod and verify_compiled construct a non-empty one, so holding a CompiledMod is the proof that its bytecode came from the pinned compiler for this package.
class CompiledMod {
 public:
  [[nodiscard]] const std::string &package_identity() const { return identity_; }
  [[nodiscard]] const std::vector<CompiledScript> &scripts() const { return scripts_; }
  // SHA-256 over the compiler pin, the package identity and each script as `path source_sha256 bytecode_sha256` in path order. Recorded at build time; the loader compares against that record.
  [[nodiscard]] const std::string &digest() const { return digest_; }
  // Stored form ("SEGLUAUC1"); only verify_compiled reads it back.
  [[nodiscard]] std::vector<std::uint8_t> serialize() const;
  [[nodiscard]] bool empty() const { return scripts_.empty(); }

 private:
  friend struct CompiledAccess;
  std::string identity_, digest_;
  std::vector<CompiledScript> scripts_;
};

struct CompileResult {
  bool ok = false;
  mod::Rejection rejection;            // E_LUAU_SYNTAX | E_LUAU_COMPILE_LIMIT | E_LUAU_BYTECODE | E_LUAU_COMPILED_MISMATCH
  std::optional<Diagnostic> diagnostic;   // E_LUAU_SYNTAX only
  CompiledMod compiled;
};

// Compiles every `luau-source` payload of a package the reader accepted. Deterministic: equal packages give equal bytecode and digest. A package without Luau yields ok with an empty CompiledMod.
[[nodiscard]] CompileResult compile_mod(const mod::Package &package);
// Compiles one source text (used by compile_mod; exposed for diagnostics tests). `path` only labels diagnostics. On success the bytecode is in `bytecode`.
struct SourceCompile {
  bool ok = false;
  Diagnostic diagnostic;
  std::vector<std::uint8_t> bytecode;
};
[[nodiscard]] SourceCompile compile_source(std::string_view path, std::string_view source);
// Accepts previously stored compiled bytes ONLY if they are exactly what compile_mod produced for `package`: well-formed, the same package identity, one entry per luau-source payload with the payload's
// source digest, every bytecode digest recomputed, and the overall digest equal to `expected_digest` (the value recorded at build time, held outside the artifact). Anything else is E_LUAU_COMPILED_MISMATCH.
[[nodiscard]] CompileResult verify_compiled(const mod::Package &package, std::span<const std::uint8_t> stored, std::string_view expected_digest);

// The digest line of a stored compiled output ("" when the header is malformed). Used by the parent of the compile child, which then calls verify_compiled with it: that re-derives every bytecode and
// total digest, so a child that returned inconsistent bytes is rejected.
[[nodiscard]] std::string stored_digest(std::span<const std::uint8_t> stored);

// ---- restricted VM ----
struct Limits {
  std::uint64_t memory_bytes = mod::kLuauMemoryPolicyMax;   // hard quota over every VM allocation
  std::uint64_t callback_units = mod::kLuauBudgetPolicyMax;  // budget units (interrupt safepoints: calls and loop back-edges) per callback
  std::uint64_t frame_units = 400000;                        // per mod per frame
  unsigned call_depth = 200;
  unsigned callback_watchdog_ms = 4, frame_watchdog_ms = 8;  // 0 disables; backstop only
};
inline constexpr std::uint64_t kFrameUnitsPolicyMax = 400000;
inline constexpr unsigned kCallDepthPolicyMax = 200;
[[nodiscard]] Limits policy_limits();
// The policy limits lowered by a package's `limits` request (never raised: the reader already rejected a request above policy).
[[nodiscard]] Limits limits_for(const mod::Manifest &manifest);

enum class Fault { none, error, budget, memory, stack, watchdog };
[[nodiscard]] const char *fault_name(Fault fault);

struct CallResult {
  bool ok = false;
  Fault fault = Fault::none;
  std::string message;          // bounded, no host paths or addresses
  std::uint64_t units = 0;      // budget units consumed by this call (deterministic)
};

struct VmImpl;

class Vm {
 public:
  // `package_identity` is the 64 hex digit identity (seeds math.random). Null when the VM cannot be created.
  [[nodiscard]] static std::unique_ptr<Vm> create(std::string_view package_identity, const Limits &limits);
  Vm(const Vm &) = delete;
  Vm &operator=(const Vm &) = delete;
  ~Vm();

  // Runs each script's main chunk, in path order, in this mod's sandboxed environment (the pseudo-callback "load": frame 0, callback index 0xFFFFFFFF). The CompiledMod must belong to the identity given at create.
  CallResult load(const CompiledMod &compiled);
  // Calls the global function `name` (no arguments, results dropped) as callback `callback_index` of `frame`. The deterministic budget restarts per call; the frame budget restarts when `frame` changes.
  CallResult call(std::string_view name, std::uint64_t frame, std::uint32_t callback_index);

  // A fault disables the mod for the rest of the VM's life (no pcall can clear it); every later load/call fails with the original fault.
  [[nodiscard]] bool disabled() const;
  [[nodiscard]] Fault fault() const;
  // A watchdog fault depends on host timing: the session is marked non-deterministic and determinism tests must fail on it.
  [[nodiscard]] bool nondeterministic() const;
  [[nodiscard]] std::uint64_t memory_in_use() const;
  // A global as "nil" | "boolean:<v>" | "number:<v>" | "string:<v>" | "<type>"; read-only observation for tests and diagnostics (copy-out).
  [[nodiscard]] std::string describe_global(std::string_view name) const;
  // Destroys the VM and returns the bytes the allocator still held afterwards (0 when teardown is leak free). Idempotent; the destructor calls it.
  std::uint64_t close();

 private:
  explicit Vm(std::unique_ptr<VmImpl> impl);
  std::unique_ptr<VmImpl> impl_;
};

}  // namespace segarecomp::luau
