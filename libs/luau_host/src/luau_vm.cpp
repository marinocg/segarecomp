// SEG-137-T002 (ADR 0294): the restricted per-mod Luau VM. See luau_host.hpp and docs/architecture/luau-trust-policy.md.
#include "segarecomp/luau_api.hpp"
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
#include <set>

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
    case Fault::capability: return "capability";
    case Fault::range: return "range";
    case Fault::phase: return "phase";
    case Fault::state: return "state";
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

enum class Phase { none, load, reset, render };

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
  // host API (luau_api.hpp); `granted` empty and `has_host` false = no `sega` table
  bool has_host = false;
  std::set<std::string> granted;
  Phase phase = Phase::none;
  const FrameInput *input = nullptr;   // valid only inside run_callback
  Output staged;                       // discarded unless the callback succeeds
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

// An integer argument of math.random, range-checked so the result never depends on the host's out-of-range conversion.
int random_arg(lua_State *L, int n) {
  const double v = luaL_checknumber(L, n);
  if (!(v >= -2147483648.0 && v <= 2147483647.0) || v != std::floor(v)) luaL_argerror(L, n, "number has no integer representation");
  return static_cast<int>(v);
}

// tostring without host addresses: tables, functions, userdata, buffers and threads render as their type name (unless the user's own __tostring decides).
int safe_tostring(lua_State *L) {
  luaL_checkany(L, 1);
  switch (lua_type(L, 1)) {
    case LUA_TTABLE:
    case LUA_TUSERDATA:
      if (luaL_getmetafield(L, 1, "__tostring") != 0) {
        lua_pop(L, 1);
        luaL_tolstring(L, 1, nullptr);
        return 1;
      }
      [[fallthrough]];
    case LUA_TFUNCTION:
    case LUA_TLIGHTUSERDATA:
    case LUA_TTHREAD:
    case LUA_TBUFFER:
      lua_pushstring(L, luaL_typename(L, 1));
      return 1;
    default: luaL_tolstring(L, 1, nullptr); return 1;
  }
}

int math_random(lua_State *L) {
  VmImpl *im = impl_of(L);
  const double r = static_cast<double>(next_random(im) >> 11U) * (1.0 / 9007199254740992.0);   // [0, 1)
  switch (lua_gettop(L)) {
    case 0: lua_pushnumber(L, r); return 1;
    case 1: {
      const int hi = random_arg(L, 1);
      luaL_argcheck(L, 1 <= hi, 1, "interval is empty");
      lua_pushnumber(L, std::floor(r * hi) + 1);
      return 1;
    }
    case 2: {
      const int lo = random_arg(L, 1), hi = random_arg(L, 2);
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

void install_host(lua_State *L, VmImpl *im);   // defined below with the bindings

// Builds the restricted globals on the main state: only the whitelisted libraries are opened, everything else the base library registered is removed, then the library tables are frozen.
void build_globals(lua_State *L, VmImpl *im) {
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
  lua_pushcfunction(L, safe_tostring, "tostring");
  lua_setfield(L, LUA_GLOBALSINDEX, "tostring");
  lua_pushcfunction(L, safe_getmetatable, "getmetatable");
  lua_setfield(L, LUA_GLOBALSINDEX, "getmetatable");
  lua_getfield(L, LUA_GLOBALSINDEX, "math");
  lua_pushcfunction(L, math_random, "random");
  lua_setfield(L, -2, "random");
  lua_pushcfunction(L, math_randomseed, "randomseed");
  lua_setfield(L, -2, "randomseed");
  lua_pop(L, 1);
  if (im->has_host) install_host(L, im);
  freeze_globals(L);
}


// ---- the `sega` host API (luau_api.hpp, policy section 5) ----
// Every binding validates its own arguments and answers a violation with a sticky fault (range / phase / budget / capability): a script cannot catch it with pcall and continue.
[[noreturn]] void binding_fault(lua_State *L, Fault f, const std::string &message) {
  set_fault(impl_of(L), f, message);
  raise(L, message.c_str());
}

void require_phase(lua_State *L, Phase want, const char *name) {
  if (impl_of(L)->phase != want) binding_fault(L, Fault::phase, std::string(name) + " is not available in this phase");
}

void require_args(lua_State *L, int count, const char *name) {
  if (lua_gettop(L) != count) binding_fault(L, Fault::range, std::string(name) + " takes " + std::to_string(count) + " argument(s)");
}

// An integer argument in [lo, hi]: a non-number, NaN, infinity, fraction or out-of-range value is a range fault (never silently converted).
std::int64_t int_arg(lua_State *L, int n, std::int64_t lo, std::int64_t hi, const char *name) {
  if (lua_type(L, n) != LUA_TNUMBER) binding_fault(L, Fault::range, std::string("argument ") + std::to_string(n) + " of " + name + " must be a number");
  const double v = lua_tonumber(L, n);
  if (!std::isfinite(v) || v != std::floor(v) || v < static_cast<double>(lo) || v > static_cast<double>(hi))
    binding_fault(L, Fault::range, std::string("argument ") + std::to_string(n) + " of " + name + " is out of range");
  return static_cast<std::int64_t>(v);
}

std::string_view str_arg(lua_State *L, int n, const char *name) {
  if (lua_type(L, n) != LUA_TSTRING) binding_fault(L, Fault::range, std::string("argument ") + std::to_string(n) + " of " + name + " must be a string");
  std::size_t len = 0;
  const char *p = lua_tolstring(L, n, &len);
  return {p, len};
}

void note(VmImpl *im, const char *cause, std::string message) {
  if (im->staged.notes.size() < kNotesMax) im->staged.notes.push_back({cause, std::move(message)});
}

// Printable ASCII only: control bytes and non-ASCII become '?', so a diagnostic or an overlay never carries raw script bytes.
std::string printable(std::string_view text, std::size_t limit, bool &truncated) {
  truncated = text.size() > limit;
  std::string out(text.substr(0, limit));
  for (char &c : out)
    if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) c = '?';
  return out;
}

int sega_log(lua_State *L) {
  VmImpl *im = impl_of(L);
  require_args(L, 1, "sega.log");
  const std::string_view text = str_arg(L, 1, "sega.log");
  if (im->staged.log.size() >= kLogLinesPerCallback) binding_fault(L, Fault::budget, "sega.log line limit exceeded");
  bool truncated = false;
  im->staged.log.push_back(printable(text, kLogLineBytes, truncated));
  if (truncated) note(im, "log_truncated", "a sega.log line was cut to 256 bytes");
  return 0;
}

int sega_frame_value(lua_State *L) {
  VmImpl *im = impl_of(L);
  require_phase(L, Phase::render, "sega.frame.value");
  require_args(L, 1, "sega.frame.value");
  const std::string_view name = str_arg(L, 1, "sega.frame.value");
  if (name.empty() || name.size() > kFrameValueNameBytes) binding_fault(L, Fault::range, "argument 1 of sega.frame.value is not a valid value name");
  if (im->input != nullptr)
    for (const FrameValue &v : im->input->values)
      if (v.name == name) {
        lua_pushnumber(L, static_cast<double>(v.value));
        return 1;
      }
  lua_pushnil(L);
  return 1;
}

void add_op(lua_State *L, OverlayOp op) {
  VmImpl *im = impl_of(L);
  if (im->staged.overlay.size() >= kOverlayOpsPerCallback) binding_fault(L, Fault::budget, "sega.overlay request limit exceeded");
  im->staged.overlay.push_back(std::move(op));
}

constexpr std::int64_t kCoord = 1 << 20;

int sega_overlay_rect(lua_State *L) {
  VmImpl *im = impl_of(L);
  require_phase(L, Phase::render, "sega.overlay.rect");
  require_args(L, 5, "sega.overlay.rect");
  const std::int64_t x = int_arg(L, 1, -kCoord, kCoord, "sega.overlay.rect"), y = int_arg(L, 2, -kCoord, kCoord, "sega.overlay.rect");
  const std::int64_t w = int_arg(L, 3, 0, kCoord, "sega.overlay.rect"), h = int_arg(L, 4, 0, kCoord, "sega.overlay.rect");
  const std::int64_t rgba = int_arg(L, 5, 0, 4294967295LL, "sega.overlay.rect");
  const std::int64_t x0 = std::max<std::int64_t>(x, 0), y0 = std::max<std::int64_t>(y, 0);
  const std::int64_t x1 = std::min<std::int64_t>(x + w, kOverlayWidth), y1 = std::min<std::int64_t>(y + h, kOverlayHeight);
  if (x1 <= x0 || y1 <= y0) {
    note(im, "overlay_clipped", "a sega.overlay.rect has no visible area");
    return 0;
  }
  if (x0 != x || y0 != y || x1 - x0 != w || y1 - y0 != h) note(im, "overlay_clamped", "a sega.overlay.rect was clamped to the overlay");
  OverlayOp op;
  op.kind = OverlayOp::Kind::rect;
  op.x = static_cast<int>(x0);
  op.y = static_cast<int>(y0);
  op.w = static_cast<int>(x1 - x0);
  op.h = static_cast<int>(y1 - y0);
  op.rgba = static_cast<std::uint32_t>(rgba);
  add_op(L, std::move(op));
  return 0;
}

int sega_overlay_text(lua_State *L) {
  VmImpl *im = impl_of(L);
  require_phase(L, Phase::render, "sega.overlay.text");
  require_args(L, 4, "sega.overlay.text");
  const std::int64_t x = int_arg(L, 1, -kCoord, kCoord, "sega.overlay.text"), y = int_arg(L, 2, -kCoord, kCoord, "sega.overlay.text");
  const std::string_view text = str_arg(L, 3, "sega.overlay.text");
  const std::int64_t rgba = int_arg(L, 4, 0, 4294967295LL, "sega.overlay.text");
  const std::int64_t cx = std::clamp<std::int64_t>(x, 0, kOverlayWidth - 1), cy = std::clamp<std::int64_t>(y, 0, kOverlayHeight - 1);
  if (cx != x || cy != y) note(im, "overlay_clamped", "a sega.overlay.text origin was clamped to the overlay");
  OverlayOp op;
  op.kind = OverlayOp::Kind::text;
  op.x = static_cast<int>(cx);
  op.y = static_cast<int>(cy);
  bool truncated = false;
  op.text = printable(text, kOverlayTextBytes, truncated);
  if (truncated) note(im, "text_truncated", "a sega.overlay.text string was cut to 64 bytes");
  op.rgba = static_cast<std::uint32_t>(rgba);
  add_op(L, std::move(op));
  return 0;
}

// __index of every `sega` namespace table: a name that exists in the API list but whose capability was not declared is a capability fault the moment the script reaches for it (the binding is absent);
// any other missing key is plain nil. The upvalue is the namespace prefix.
int sega_missing(lua_State *L) {
  VmImpl *im = impl_of(L);
  const char *prefix = lua_tostring(L, lua_upvalueindex(1));
  if (lua_type(L, 2) == LUA_TSTRING && prefix != nullptr) {
    const ApiEntry *e = find_api(std::string(prefix) + "." + lua_tostring(L, 2));
    if (e != nullptr && !e->capability.empty() && im->granted.count(std::string(e->capability)) == 0)
      binding_fault(L, Fault::capability, "capability not declared: " + std::string(e->capability));
  }
  lua_pushnil(L);
  return 1;
}

void guard_namespace(lua_State *L, const char *prefix) {   // table on top of the stack
  lua_newtable(L);
  lua_pushstring(L, prefix);
  lua_pushcclosurek(L, sega_missing, "index", 1, nullptr);
  lua_setfield(L, -2, "__index");
  lua_pushliteral(L, "locked");
  lua_setfield(L, -2, "__metatable");
  lua_setreadonly(L, -1, true);
  lua_setmetatable(L, -2);
}

void bind(lua_State *L, VmImpl *im, std::string_view name, lua_CFunction fn) {   // `sega.<name>` leaf on the table on top of the stack, only when its capability is granted
  const ApiEntry *e = find_api(name);
  if (e == nullptr || (!e->capability.empty() && im->granted.count(std::string(e->capability)) == 0)) return;
  const std::string leaf(name.substr(name.rfind('.') + 1));
  lua_pushcfunction(L, fn, leaf.c_str());
  lua_setfield(L, -2, leaf.c_str());
}

void install_host(lua_State *L, VmImpl *im) {
  lua_newtable(L);   // sega
  bind(L, im, "sega.log", sega_log);
  lua_newtable(L);   // sega.frame
  bind(L, im, "sega.frame.value", sega_frame_value);
  guard_namespace(L, "sega.frame");
  lua_setreadonly(L, -1, true);
  lua_setfield(L, -2, "frame");
  lua_newtable(L);   // sega.overlay
  bind(L, im, "sega.overlay.rect", sega_overlay_rect);
  bind(L, im, "sega.overlay.text", sega_overlay_text);
  if (im->granted.count("overlay.rgba") != 0) {
    lua_pushnumber(L, kOverlayWidth);
    lua_setfield(L, -2, "width");
    lua_pushnumber(L, kOverlayHeight);
    lua_setfield(L, -2, "height");
  }
  guard_namespace(L, "sega.overlay");
  lua_setreadonly(L, -1, true);
  lua_setfield(L, -2, "overlay");
  guard_namespace(L, "sega");
  lua_setreadonly(L, -1, true);
  lua_setfield(L, LUA_GLOBALSINDEX, "sega");
}

// ---- mod state: canonical serialization of the one value save() returns (phases section 6.2) ----
// bytes: 1 false | 2 true | 3 number (f64 LE) | 4 string (u32 LE length, bytes) | 5 table (u32 LE entries, then key/value pairs; keys are numbers ascending, then strings bytewise ascending)
struct StateKey {
  bool is_number = false;
  double number = 0;
  std::string text;
  std::vector<std::uint8_t> value;   // serialized value
  bool operator<(const StateKey &o) const {
    if (is_number != o.is_number) return is_number;
    return is_number ? number < o.number : text < o.text;
  }
};

void put_u32(std::vector<std::uint8_t> &out, std::uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8U * i)));
}
void put_number(std::vector<std::uint8_t> &out, double v) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &v, sizeof bits);
  for (unsigned i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(bits >> (8U * i)));
}
void put_key(std::vector<std::uint8_t> &out, const StateKey &k) {
  if (k.is_number) {
    out.push_back(3);
    put_number(out, k.number);
  } else {
    out.push_back(4);
    put_u32(out, static_cast<std::uint32_t>(k.text.size()));
    out.insert(out.end(), k.text.begin(), k.text.end());
  }
}

bool serialize_value(lua_State *L, int index, unsigned depth, std::vector<std::uint8_t> &out, std::string &error) {
  index = lua_absindex(L, index);
  if (out.size() > kStateBytesMax) {
    error = "state exceeds the size limit";
    return false;
  }
  switch (lua_type(L, index)) {
    case LUA_TBOOLEAN: out.push_back(lua_toboolean(L, index) != 0 ? 2 : 1); return true;
    case LUA_TNUMBER: {
      const double v = lua_tonumber(L, index);
      if (!std::isfinite(v)) {
        error = "state contains a number that is not finite";
        return false;
      }
      out.push_back(3);
      put_number(out, v);
      return true;
    }
    case LUA_TSTRING: {
      std::size_t len = 0;
      const char *p = lua_tolstring(L, index, &len);
      if (len > kStateBytesMax) {
        error = "state exceeds the size limit";
        return false;
      }
      out.push_back(4);
      put_u32(out, static_cast<std::uint32_t>(len));
      out.insert(out.end(), p, p + len);
      return true;
    }
    case LUA_TTABLE: {
      if (depth >= kStateDepthMax) {
        error = "state is nested too deeply (or cyclic)";
        return false;
      }
      std::vector<StateKey> entries;
      std::size_t pending = 0;
      lua_pushnil(L);
      while (lua_next(L, index) != 0) {
        StateKey k;
        const int kt = lua_type(L, -2);
        if (kt == LUA_TNUMBER) {
          k.is_number = true;
          k.number = lua_tonumber(L, -2);
          if (!std::isfinite(k.number)) {
            error = "state contains a key that is not finite";
            return false;
          }
        } else if (kt == LUA_TSTRING) {
          k.text = lua_tostring(L, -2);
        } else {
          error = "state contains a key that is neither a number nor a string";
          return false;
        }
        if (!serialize_value(L, -1, depth + 1, k.value, error)) return false;
        pending += k.value.size() + k.text.size() + 9;
        if (pending > kStateBytesMax) {
          error = "state exceeds the size limit";
          return false;
        }
        entries.push_back(std::move(k));
        lua_pop(L, 1);
      }
      std::sort(entries.begin(), entries.end());
      out.push_back(5);
      put_u32(out, static_cast<std::uint32_t>(entries.size()));
      for (const StateKey &k : entries) {
        put_key(out, k);
        out.insert(out.end(), k.value.begin(), k.value.end());
      }
      return true;
    }
    default: error = std::string("state contains a ") + luaL_typename(L, index); return false;
  }
}

struct StateReader {
  std::span<const std::uint8_t> data;
  std::size_t pos = 0;
  std::string error;
  bool fail(const char *why) {
    if (error.empty()) error = why;
    return false;
  }
  bool u8(std::uint8_t &v) {
    if (pos >= data.size()) return fail("state is truncated");
    v = data[pos++];
    return true;
  }
  bool u32(std::uint32_t &v) {
    if (data.size() - pos < 4) return fail("state is truncated");
    v = 0;
    for (unsigned i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(data[pos + i]) << (8U * i);
    pos += 4;
    return true;
  }
  bool f64(double &v) {
    if (data.size() - pos < 8) return fail("state is truncated");
    std::uint64_t bits = 0;
    for (unsigned i = 0; i < 8; ++i) bits |= static_cast<std::uint64_t>(data[pos + i]) << (8U * i);
    pos += 8;
    std::memcpy(&v, &bits, sizeof v);
    if (!std::isfinite(v)) return fail("state contains a number that is not finite");
    return true;
  }
  bool str(std::string &s) {
    std::uint32_t len = 0;
    if (!u32(len)) return false;
    if (len > data.size() - pos) return fail("state is truncated");
    s.assign(reinterpret_cast<const char *>(data.data() + pos), len);
    pos += len;
    return true;
  }
};

// Parses one value and pushes it. Keys must be strictly increasing in the canonical order, which also rejects duplicates and makes the encoding unique.
bool deserialize_value(lua_State *L, StateReader &in, unsigned depth) {
  std::uint8_t tag = 0;
  if (!in.u8(tag)) return false;
  switch (tag) {
    case 1: lua_pushboolean(L, 0); return true;
    case 2: lua_pushboolean(L, 1); return true;
    case 3: {
      double v = 0;
      if (!in.f64(v)) return false;
      lua_pushnumber(L, v);
      return true;
    }
    case 4: {
      std::string s;
      if (!in.str(s)) return false;
      lua_pushlstring(L, s.data(), s.size());
      return true;
    }
    case 5: {
      if (depth >= kStateDepthMax) return in.fail("state is nested too deeply");
      std::uint32_t count = 0;
      if (!in.u32(count)) return false;
      if (count > (in.data.size() - in.pos) / 2U) return in.fail("state is truncated");
      lua_createtable(L, 0, static_cast<int>(std::min<std::uint32_t>(count, 64U)));
      StateKey previous;
      for (std::uint32_t i = 0; i < count; ++i) {
        StateKey k;
        std::uint8_t kt = 0;
        if (!in.u8(kt)) return false;
        if (kt == 3) {
          k.is_number = true;
          if (!in.f64(k.number)) return false;
        } else if (kt == 4) {
          if (!in.str(k.text)) return false;
        } else {
          return in.fail("state contains a key that is neither a number nor a string");
        }
        if (i > 0 && !(previous < k)) return in.fail("state keys are not in canonical order");
        if (!lua_checkstack(L, 4)) return in.fail("state is too large");
        if (k.is_number) lua_pushnumber(L, k.number);
        else lua_pushlstring(L, k.text.data(), k.text.size());
        if (!deserialize_value(L, in, depth + 1)) return false;
        lua_settable(L, -3);
        previous = std::move(k);
      }
      return true;
    }
    default: return in.fail("state contains an unknown value tag");
  }
}

CallResult state_fault(VmImpl *im, const std::string &message) {
  set_fault(im, Fault::state, message);
  return {false, im->fault, im->fault_message, 0};
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
CallResult run(VmImpl *im, std::uint64_t frame, std::uint32_t index, int nargs = 0, Phase phase = Phase::none, int nresults = 0) {
  CallResult r;
  im->phase = phase;
  im->staged = Output{};
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
  const int status = lua_pcall(im->env, nargs, nresults, 0);
  im->running = false;
  im->phase = Phase::none;
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

std::unique_ptr<Vm> Vm::create(std::string_view package_identity, const Limits &limits) { return create_vm(package_identity, limits, nullptr); }

std::unique_ptr<Vm> Vm::create(std::string_view package_identity, const Limits &limits, const HostConfig &host) { return create_vm(package_identity, limits, &host); }

std::unique_ptr<Vm> Vm::create_vm(std::string_view package_identity, const Limits &limits, const HostConfig *host) {
  if (package_identity.size() != 64) return nullptr;
  auto im = std::make_unique<VmImpl>();
  im->limits = limits;
  if (host != nullptr) {
    im->has_host = true;
    im->granted.insert(host->capabilities.begin(), host->capabilities.end());
  }
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
    build_globals(im->main, im.get());
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
    last = run(im, 0, 0xFFFFFFFFU, 0, Phase::load);
    if (!last.ok) return last;
  }
  return last;
}

CallResult Vm::call(std::string_view name, std::uint64_t frame, std::uint32_t callback_index) {
  VmImpl *im = impl_.get();
  if (im == nullptr || im->closed) return {false, Fault::error, "the VM is closed", 0};
  if (im->fault != Fault::none) return {false, im->fault, im->fault_message, 0};
  const std::string key(name);
  try {
    lua_getglobal(im->env, key.c_str());   // interns the name: may allocate, so it may hit the quota outside any protected call
  } catch (...) {
    set_fault(im, Fault::memory, "memory quota exceeded");
    return {false, Fault::memory, im->fault_message, 0};
  }
  if (lua_type(im->env, -1) != LUA_TFUNCTION) {
    lua_pop(im->env, 1);
    set_fault(im, Fault::error, "the callback is not a function");
    return {false, Fault::error, im->fault_message, 0};
  }
  return run(im, frame, callback_index);
}

CallResult Vm::run_callback(std::string_view name, const FrameInput &input, std::uint32_t callback_index, Output &out) {
  VmImpl *im = impl_.get();
  out = Output{};
  if (im == nullptr || im->closed || !im->has_host) return {false, Fault::error, "the VM has no host API", 0};
  if (im->fault != Fault::none) return {false, im->fault, im->fault_message, 0};
  const bool render = name == "render";
  if (!render && name != "reset") return {false, Fault::error, "unknown lifecycle callback", 0};
  const std::string key(name);
  try {
    lua_getglobal(im->env, key.c_str());
  } catch (...) {
    set_fault(im, Fault::memory, "memory quota exceeded");
    return {false, Fault::memory, im->fault_message, 0};
  }
  if (lua_type(im->env, -1) != LUA_TFUNCTION) {   // an undefined callback is skipped
    lua_pop(im->env, 1);
    CallResult skipped;
    skipped.ok = true;
    return skipped;
  }
  if (render) lua_pushnumber(im->env, static_cast<double>(input.number));
  im->input = &input;
  CallResult r = run(im, render ? input.number : 0, callback_index, render ? 1 : 0, render ? Phase::render : Phase::reset);
  im->input = nullptr;
  if (r.ok) out = std::move(im->staged);
  im->staged = Output{};
  return r;
}

StateResult Vm::save_state() {
  VmImpl *im = impl_.get();
  StateResult r;
  if (im == nullptr || im->closed) {
    r.fault = Fault::error;
    r.message = "the VM is closed";
    return r;
  }
  if (im->fault != Fault::none) {
    r.fault = im->fault;
    r.message = im->fault_message;
    return r;
  }
  try {
    lua_getglobal(im->env, "save");
  } catch (...) {
    r.fault = Fault::memory;
    set_fault(im, Fault::memory, "memory quota exceeded");
    r.message = im->fault_message;
    return r;
  }
  if (lua_type(im->env, -1) != LUA_TFUNCTION) {   // no save(): the mod has no state
    lua_pop(im->env, 1);
    r.ok = true;
    return r;
  }
  const CallResult c = run(im, im->frame, 2, 0, Phase::none, 1);
  if (!c.ok) {
    r.fault = c.fault;
    r.message = c.message;
    return r;
  }
  std::string error;
  std::vector<std::uint8_t> blob;
  const bool serialized = lua_type(im->env, -1) != LUA_TNIL ? serialize_value(im->env, -1, 0, blob, error) : (error = "save() returned nil", false);
  lua_pop(im->env, 1);
  if (!serialized || blob.size() > kStateBytesMax) {
    set_fault(im, Fault::state, serialized ? "state exceeds the size limit" : error);
    r.fault = Fault::state;
    r.message = im->fault_message;
    return r;
  }
  r.ok = true;
  r.blob = std::move(blob);
  return r;
}

CallResult Vm::load_state(std::span<const std::uint8_t> blob) {
  VmImpl *im = impl_.get();
  if (im == nullptr || im->closed) return {false, Fault::error, "the VM is closed", 0};
  if (im->fault != Fault::none) return {false, im->fault, im->fault_message, 0};
  if (blob.empty()) return {true, Fault::none, "", 0};
  if (blob.size() > kStateBytesMax) return state_fault(im, "state exceeds the size limit");
  try {
    lua_getglobal(im->env, "load");
    if (lua_type(im->env, -1) != LUA_TFUNCTION) {
      lua_pop(im->env, 1);
      return state_fault(im, "the mod has no load() for the saved state");
    }
    StateReader in;
    in.data = blob;
    if (!lua_checkstack(im->env, 16) || !deserialize_value(im->env, in, 0) || in.pos != blob.size()) {
      lua_settop(im->env, 0);
      return state_fault(im, in.error.empty() ? "state has trailing bytes" : in.error);
    }
  } catch (...) {
    return state_fault(im, "memory quota exceeded");
  }
  return run(im, im->frame, 3, 1, Phase::none);
}

bool Vm::disabled() const { return impl_ == nullptr || impl_->closed || impl_->fault != Fault::none; }
Fault Vm::fault() const { return impl_ != nullptr ? impl_->fault : Fault::none; }
bool Vm::nondeterministic() const { return impl_ != nullptr && impl_->nondeterministic; }
std::uint64_t Vm::memory_in_use() const { return impl_ != nullptr ? impl_->used : 0; }

std::string Vm::describe_global(std::string_view name) const {
  VmImpl *im = impl_.get();
  if (im == nullptr || im->closed) return "closed";
  const std::string key(name);
  try {
    lua_getglobal(im->env, key.c_str());
  } catch (...) {
    return "memory";
  }
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
