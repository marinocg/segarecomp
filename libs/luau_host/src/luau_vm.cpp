// SEG-137-T002 (ADR 0294): the restricted per-mod Luau VM. See luau_host.hpp and docs/architecture/luau-trust-policy.md.
#include "segarecomp/luau_host.hpp"

#include "segarecomp/sha256.hpp"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Native code generation is not built (third_party/luau/CMakeLists.txt); this makes the claim a compile-time fact for every translation unit that includes the VM.
static_assert(SEGARECOMP_LUAU_CODEGEN == 0, "Luau native code generation must not be built");

namespace segarecomp::luau {

using Clock = std::chrono::steady_clock;

const char *fault_name(Fault fault) {
  switch (fault) {
    case Fault::none: return "none";
    case Fault::error: return "error";
    case Fault::budget: return "budget";
    case Fault::memory: return "memory";
    case Fault::stack: return "stack";
    case Fault::watchdog: return "watchdog";
  }
  return "error";
}

Limits policy_limits() { return Limits{}; }

Limits limits_for(const mod::Manifest &manifest) {
  Limits l = policy_limits();
  if (manifest.luau_memory_bytes != 0) l.memory_bytes = std::min(l.memory_bytes, manifest.luau_memory_bytes);
  if (manifest.luau_budget_units != 0) l.callback_units = std::min(l.callback_units, manifest.luau_budget_units);
  return l;
}

struct VmImpl {
  Limits limits;
  std::array<std::uint8_t, 32> identity{};
  std::string identity_hex;
  lua_State *main = nullptr, *env = nullptr;   // `env` is the mod's sandboxed thread (its own globals table)
  std::uint64_t used = 0;
  bool out_of_memory = false;
  bool running = false;
  Fault fault = Fault::none;
  std::string fault_message;
  bool nondeterministic = false;
  std::uint64_t callback_units = 0, frame_units = 0, frame = ~std::uint64_t{0};
  Clock::time_point callback_start, frame_start;
  double frame_ms = 0;
  std::uint64_t rng = 0;
  bool closed = false;
  std::uint64_t leaked = 0;
};

namespace {

constexpr std::size_t kMaxMessage = 200;

VmImpl *impl_of(lua_State *L) { return static_cast<VmImpl *>(lua_callbacks(L)->userdata); }

void *allocator(void *ud, void *ptr, std::size_t osize, std::size_t nsize) {
  auto *im = static_cast<VmImpl *>(ud);
  const std::size_t old = ptr != nullptr ? osize : 0;
  if (nsize == 0) {
    if (ptr != nullptr) {
      im->used -= old;
      std::free(ptr);
    }
    return nullptr;
  }
  if (nsize > old && im->used - old + nsize > im->limits.memory_bytes) {   // a shrink always succeeds; growth is checked against the quota
    im->out_of_memory = true;
    return nullptr;
  }
  void *p = std::realloc(ptr, nsize);
  if (p == nullptr) return nullptr;
  im->used = im->used - old + nsize;
  return p;
}

void set_fault(VmImpl *im, Fault f, std::string message) {
  if (im->fault != Fault::none) return;
  im->fault = f;
  im->fault_message = std::move(message);
  if (f == Fault::watchdog) im->nondeterministic = true;
}

[[noreturn]] void raise(lua_State *L, const char *text) { luaL_error(L, "%s", text); }

// The deterministic budget. Called by the VM at every safepoint (function call, loop back-edge; gc >= 0 is a collector step and is not a budget unit). A fault is sticky: once one is set every later
// safepoint raises again, so a script cannot swallow the error with pcall and keep running.
void interrupt(lua_State *L, int gc) {
  if (gc >= 0) return;
  VmImpl *im = impl_of(L);
  if (im == nullptr || !im->running) return;
  if (im->fault != Fault::none) raise(L, "mod disabled");
  ++im->callback_units;
  ++im->frame_units;
  if (im->callback_units > im->limits.callback_units || im->frame_units > im->limits.frame_units) {
    set_fault(im, Fault::budget, "instruction budget exceeded");
    raise(L, "instruction budget exceeded");
  }
  if (static_cast<unsigned>(lua_stackdepth(L)) > im->limits.call_depth) {
    set_fault(im, Fault::stack, "call depth exceeded");
    raise(L, "call depth exceeded");
  }
  if ((im->callback_units & 255U) == 0) {   // watchdog: host-hang backstop, never part of the determinism guarantee
    const auto now = Clock::now();
    const double cb = std::chrono::duration<double, std::milli>(now - im->callback_start).count();
    const double fr = im->frame_ms + std::chrono::duration<double, std::milli>(now - im->frame_start).count();
    if ((im->limits.callback_watchdog_ms != 0 && cb > im->limits.callback_watchdog_ms) || (im->limits.frame_watchdog_ms != 0 && fr > im->limits.frame_watchdog_ms)) {
      set_fault(im, Fault::watchdog, "watchdog expired");
      raise(L, "watchdog expired");
    }
  }
}

// ---- deterministic math.random (policy section 3): splitmix64 seeded per callback ----
std::uint64_t next_random(VmImpl *im) {
  im->rng += 0x9E3779B97F4A7C15ULL;
  std::uint64_t z = im->rng;
  z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31U);
}

std::uint64_t seed_for(const std::array<std::uint8_t, 32> &identity, std::uint64_t frame, std::uint32_t index) {
  std::vector<std::uint8_t> in(identity.begin(), identity.end());
  for (unsigned i = 0; i < 8; ++i) in.push_back(static_cast<std::uint8_t>(frame >> (8U * i)));
  for (unsigned i = 0; i < 4; ++i) in.push_back(static_cast<std::uint8_t>(index >> (8U * i)));
  const std::string hex = sha256_hex(in);
  std::uint64_t seed = 0;
  for (unsigned i = 0; i < 8; ++i) {   // first 8 bytes of the digest, little endian
    const auto nib = [&](char c) { return static_cast<std::uint64_t>(c <= '9' ? c - '0' : c - 'a' + 10); };
    const std::uint64_t byte = nib(hex[2 * i]) * 16 + nib(hex[2 * i + 1]);
    seed |= byte << (8U * i);
  }
  return seed;
}

int math_random(lua_State *L) {
  VmImpl *im = impl_of(L);
  const double r = static_cast<double>(next_random(im) >> 11U) * (1.0 / 9007199254740992.0);   // [0, 1)
  switch (lua_gettop(L)) {
    case 0: lua_pushnumber(L, r); return 1;
    case 1: {
      const int hi = luaL_checkinteger(L, 1);
      luaL_argcheck(L, 1 <= hi, 1, "interval is empty");
      lua_pushnumber(L, std::floor(r * hi) + 1);
      return 1;
    }
    case 2: {
      const int lo = luaL_checkinteger(L, 1), hi = luaL_checkinteger(L, 2);
      luaL_argcheck(L, lo <= hi, 2, "interval is empty");
      lua_pushnumber(L, std::floor(r * (static_cast<double>(hi) - lo + 1)) + lo);
      return 1;
    }
    default: luaL_error(L, "wrong number of arguments");
  }
  return 0;
}

int math_randomseed(lua_State *L) {
  luaL_error(L, "math.randomseed is not available");
  return 0;
}

// getmetatable for user tables only: the string metatable is not reachable (policy section 3).
int safe_getmetatable(lua_State *L) {
  luaL_checkany(L, 1);
  if (!lua_istable(L, 1)) {
    lua_pushnil(L);
    return 1;
  }
  if (luaL_getmetafield(L, 1, "__metatable") != 0) return 1;   // a protected metatable answers with its guard value, like the stock function
  if (lua_getmetatable(L, 1) == 0) lua_pushnil(L);
  return 1;
}

constexpr std::string_view kBaseAllowed[] = {"assert", "error", "ipairs", "next", "pairs", "pcall", "rawequal", "rawget", "rawset", "select",
                                             "setmetatable", "getmetatable", "tonumber", "tostring", "type", "typeof", "unpack"};
constexpr std::string_view kLibraries[] = {"math", "string", "table", "bit32", "utf8", "buffer"};

bool allowed_global(std::string_view name) {
  return std::find(std::begin(kBaseAllowed), std::end(kBaseAllowed), name) != std::end(kBaseAllowed) ||
         std::find(std::begin(kLibraries), std::end(kLibraries), name) != std::end(kLibraries);
}

void open_library(lua_State *L, lua_CFunction open, const char *name) {
  lua_pushcfunction(L, open, nullptr);
  lua_pushstring(L, name);
  lua_call(L, 1, 0);
}

// The stock luaL_sandbox / luaL_sandboxthread live in linit.cpp, which also opens the disallowed libraries and is therefore not compiled (third_party/luau/CMakeLists.txt); these are their
// equivalents for the libraries this host opens: every library table, the string metatable and the globals table become read-only, and each mod's thread gets its own globals proxying the frozen ones.
void freeze_globals(lua_State *L) {
  lua_pushnil(L);
  while (lua_next(L, LUA_GLOBALSINDEX) != 0) {
    if (lua_istable(L, -1)) lua_setreadonly(L, -1, true);
    lua_pop(L, 1);
  }
  lua_pushliteral(L, "");
  if (lua_getmetatable(L, -1) != 0) {
    lua_setreadonly(L, -1, true);
    lua_pop(L, 2);
  } else {
    lua_pop(L, 1);
  }
  lua_setreadonly(L, LUA_GLOBALSINDEX, true);
  lua_setsafeenv(L, LUA_GLOBALSINDEX, true);
}

void sandbox_thread(lua_State *L) {
  lua_newtable(L);
  lua_newtable(L);
  lua_pushvalue(L, LUA_GLOBALSINDEX);
  lua_setfield(L, -2, "__index");
  lua_setreadonly(L, -1, true);
  lua_setmetatable(L, -2);
  lua_replace(L, LUA_GLOBALSINDEX);
  lua_setsafeenv(L, LUA_GLOBALSINDEX, true);
}

// Builds the restricted globals on the main state: only the whitelisted libraries are opened, everything else the base library registered is removed, then the library tables are frozen.
void build_globals(lua_State *L) {
  open_library(L, luaopen_base, "");
  open_library(L, luaopen_math, LUA_MATHLIBNAME);
  open_library(L, luaopen_string, LUA_STRLIBNAME);
  open_library(L, luaopen_table, LUA_TABLIBNAME);
  open_library(L, luaopen_bit32, LUA_BITLIBNAME);
  open_library(L, luaopen_utf8, LUA_UTF8LIBNAME);
  open_library(L, luaopen_buffer, LUA_BUFFERLIBNAME);
  std::vector<std::string> remove;
  lua_pushnil(L);
  while (lua_next(L, LUA_GLOBALSINDEX) != 0) {
    lua_pop(L, 1);   // keep the key for lua_next
    if (lua_type(L, -1) == LUA_TSTRING && !allowed_global(lua_tostring(L, -1))) remove.emplace_back(lua_tostring(L, -1));
  }
  for (const std::string &name : remove) {
    lua_pushnil(L);
    lua_setfield(L, LUA_GLOBALSINDEX, name.c_str());
  }
  lua_pushcfunction(L, safe_getmetatable, "getmetatable");
  lua_setfield(L, LUA_GLOBALSINDEX, "getmetatable");
  lua_getfield(L, LUA_GLOBALSINDEX, "math");
  lua_pushcfunction(L, math_random, "random");
  lua_setfield(L, -2, "random");
  lua_pushcfunction(L, math_randomseed, "randomseed");
  lua_setfield(L, -2, "randomseed");
  lua_pop(L, 1);
  freeze_globals(L);
}

std::string bounded_message(lua_State *L) {
  const char *m = lua_tostring(L, -1);
  std::string s = m != nullptr ? m : "error";
  const std::size_t nl = s.find('\n');
  if (nl != std::string::npos) s.resize(nl);
  if (s.size() > kMaxMessage) s.resize(kMaxMessage);
  return s;
}

// Runs the function on top of `T` (already pushed) under the budget; classifies the outcome.
CallResult run(VmImpl *im, std::uint64_t frame, std::uint32_t index) {
  CallResult r;
  const auto now = Clock::now();
  if (im->frame != frame) {
    im->frame = frame;
    im->frame_units = 0;
    im->frame_ms = 0;
  }
  im->frame_start = now;
  im->callback_units = 0;
  im->callback_start = now;
  im->rng = seed_for(im->identity, frame, index);
  const std::uint64_t units_before = im->frame_units;
  im->running = true;
  const int status = lua_pcall(im->env, 0, 0, 0);
  im->running = false;
  im->frame_ms += std::chrono::duration<double, std::milli>(Clock::now() - im->frame_start).count();
  r.units = im->frame_units - units_before;
  if (im->out_of_memory) set_fault(im, Fault::memory, "memory quota exceeded");
  if (status != 0) {
    const std::string message = bounded_message(im->env);
    lua_pop(im->env, 1);
    set_fault(im, Fault::error, message);
  }
  r.ok = im->fault == Fault::none;
  r.fault = im->fault;
  r.message = im->fault_message;
  return r;
}

}  // namespace

Vm::Vm(std::unique_ptr<VmImpl> impl) : impl_(std::move(impl)) {}

Vm::~Vm() { (void)close(); }

std::unique_ptr<Vm> Vm::create(std::string_view package_identity, const Limits &limits) {
  if (package_identity.size() != 64) return nullptr;
  auto im = std::make_unique<VmImpl>();
  im->limits = limits;
  im->identity_hex = std::string(package_identity);
  for (std::size_t i = 0; i < 32; ++i) {
    const auto nib = [&](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    const int hi = nib(package_identity[2 * i]), lo = nib(package_identity[2 * i + 1]);
    if (hi < 0 || lo < 0) return nullptr;
    im->identity[i] = static_cast<std::uint8_t>(hi * 16 + lo);
  }
  im->main = lua_newstate(allocator, im.get());
  if (im->main == nullptr) return nullptr;
  lua_callbacks(im->main)->userdata = im.get();
  lua_callbacks(im->main)->interrupt = interrupt;
  try {
    build_globals(im->main);
    im->env = lua_newthread(im->main);   // anchored on the main stack for the life of the VM
    sandbox_thread(im->env);
  } catch (...) {   // quota too small to even open the libraries
    lua_close(im->main);
    return nullptr;
  }
  return std::unique_ptr<Vm>(new Vm(std::move(im)));
}

CallResult Vm::load(const CompiledMod &compiled) {
  VmImpl *im = impl_.get();
  if (im == nullptr || im->closed) return {false, Fault::error, "the VM is closed", 0};
  if (im->fault != Fault::none) return {false, im->fault, im->fault_message, 0};
  if (compiled.empty()) return {false, Fault::error, "no compiled scripts", 0};
  if (compiled.package_identity() != im->identity_hex) return {false, Fault::error, "the compiled scripts belong to another package identity", 0};   // refused, not a fault of this mod
  CallResult last;
  last.ok = true;
  for (const CompiledScript &s : compiled.scripts()) {
    const std::string chunk = "=" + s.path;
    if (luau_load(im->env, chunk.c_str(), reinterpret_cast<const char *>(s.bytecode.data()), s.bytecode.size(), 0) != 0) {
      const std::string message = bounded_message(im->env);
      lua_pop(im->env, 1);
      if (im->out_of_memory) set_fault(im, Fault::memory, "memory quota exceeded");
      else set_fault(im, Fault::error, message);
      return {false, im->fault, im->fault_message, 0};
    }
    last = run(im, 0, 0xFFFFFFFFU);
    if (!last.ok) return last;
  }
  return last;
}

CallResult Vm::call(std::string_view name, std::uint64_t frame, std::uint32_t callback_index) {
  VmImpl *im = impl_.get();
  if (im == nullptr || im->closed) return {false, Fault::error, "the VM is closed", 0};
  if (im->fault != Fault::none) return {false, im->fault, im->fault_message, 0};
  const std::string key(name);
  lua_getglobal(im->env, key.c_str());
  if (lua_type(im->env, -1) != LUA_TFUNCTION) {
    lua_pop(im->env, 1);
    set_fault(im, Fault::error, "the callback is not a function");
    return {false, Fault::error, im->fault_message, 0};
  }
  return run(im, frame, callback_index);
}

bool Vm::disabled() const { return impl_ == nullptr || impl_->closed || impl_->fault != Fault::none; }
Fault Vm::fault() const { return impl_ != nullptr ? impl_->fault : Fault::none; }
bool Vm::nondeterministic() const { return impl_ != nullptr && impl_->nondeterministic; }
std::uint64_t Vm::memory_in_use() const { return impl_ != nullptr ? impl_->used : 0; }

std::string Vm::describe_global(std::string_view name) const {
  VmImpl *im = impl_.get();
  if (im == nullptr || im->closed) return "closed";
  const std::string key(name);
  lua_getglobal(im->env, key.c_str());
  std::string out;
  switch (lua_type(im->env, -1)) {
    case LUA_TNIL: out = "nil"; break;
    case LUA_TBOOLEAN: out = lua_toboolean(im->env, -1) != 0 ? "boolean:true" : "boolean:false"; break;
    case LUA_TNUMBER: {
      char buf[64];
      std::snprintf(buf, sizeof buf, "number:%.17g", lua_tonumber(im->env, -1));
      out = buf;
      break;
    }
    case LUA_TSTRING: out = std::string("string:") + lua_tostring(im->env, -1); break;
    default: out = lua_typename(im->env, lua_type(im->env, -1)); break;
  }
  lua_pop(im->env, 1);
  return out;
}

std::uint64_t Vm::close() {
  VmImpl *im = impl_.get();
  if (im == nullptr || im->closed) return im != nullptr ? im->leaked : 0;
  im->closed = true;
  im->running = false;
  lua_close(im->main);
  im->main = im->env = nullptr;
  im->leaked = im->used;
  return im->leaked;
}

}  // namespace segarecomp::luau
