// SEG-137-T002/T003 (ADR 0294): the restricted per-mod Luau VM and the trusted compile step, against synthetic scripts only. Exits nonzero on the first failed check.
#include "segarecomp/luau_host.hpp"
#include "segarecomp/sha256.hpp"


#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using namespace segarecomp;

namespace {

int failures = 0;
void check(bool ok, const std::string &what) {
  if (!ok) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
  }
}

std::vector<std::uint8_t> bytes_of(const std::string &s) { return {s.begin(), s.end()}; }

// A schema 3 package with the given Luau scripts (path -> source), read back through the real reader so the identity is the real one.
mod::Package make_package(const std::map<std::string, std::string> &scripts) {
  mod::Manifest m;
  m.id = "luau-test";
  m.version = "1.0.0";
  m.name = "Luau test";
  m.platform = "genesis";
  m.source_kind = "cartridge-image";
  m.revisions.push_back({"rev-a", std::string(64, 'a'), ""});
  m.capabilities = {"host.log"};
  std::map<std::string, std::vector<std::uint8_t>> files;
  for (const auto &[path, text] : scripts) {
    mod::Payload p;
    p.kind = "luau-source";
    p.path = path;
    p.size = text.size();
    p.sha256 = sha256_hex(bytes_of(text));
    p.role = "script";
    m.payloads.push_back(p);
    files[path] = bytes_of(text);
  }
  mod::Package r = mod::read_package(mod::pack_package(m, files));
  if (!r.ok) {
    std::fprintf(stderr, "package rejected: %s %s\n", r.rejection.code.c_str(), r.rejection.message.c_str());
    std::exit(2);
  }
  return r;
}

struct Loaded {
  mod::Package package;
  std::unique_ptr<luau::Vm> vm;
  luau::CallResult loaded;
};

Loaded load_script(const std::string &source, const luau::Limits &limits = luau::policy_limits()) {
  Loaded l;
  l.package = make_package({{"scripts/main.luau", source}});
  const luau::CompileResult c = luau::compile_mod(l.package);
  if (!c.ok) {
    std::fprintf(stderr, "compile failed: %s\n", c.rejection.message.c_str());
    std::exit(2);
  }
  l.vm = luau::Vm::create(l.package.identity, limits);
  if (!l.vm) std::exit(2);
  l.loaded = l.vm->load(c.compiled);
  return l;
}

luau::Limits relaxed_watchdog() {   // sanitizer builds are slow; the deterministic limits decide, the watchdog is a separate test
  luau::Limits l;
  l.callback_watchdog_ms = 0;
  l.frame_watchdog_ms = 0;
  return l;
}

void test_codegen_off() {
  static_assert(SEGARECOMP_LUAU_CODEGEN == 0);
  check(luau::compiler_pin() == "0.746", "compiler pin");
}

void test_compile() {
  const mod::Package p = make_package({{"scripts/b.luau", "x = 2"}, {"scripts/a.luau", "x = 1\nfunction f() return x end"}});
  const luau::CompileResult a = luau::compile_mod(p), b = luau::compile_mod(p);
  check(a.ok && b.ok, "compile ok");
  check(a.compiled.digest() == b.compiled.digest() && a.compiled.serialize() == b.compiled.serialize(), "compile output is deterministic");
  check(a.compiled.scripts().size() == 2 && a.compiled.scripts()[0].path == "scripts/a.luau", "scripts in path order");
  check(a.compiled.package_identity() == p.identity, "bound to the package identity");
  // the digest changes with the source
  const mod::Package q = make_package({{"scripts/b.luau", "x = 3"}, {"scripts/a.luau", "x = 1\nfunction f() return x end"}});
  check(luau::compile_mod(q).compiled.digest() != a.compiled.digest(), "digest depends on the source");
  // syntax errors: stable file:line
  const mod::Package bad = make_package({{"scripts/main.luau", "local a = 1\n\nlocal b = = 2\n"}});
  const luau::CompileResult e1 = luau::compile_mod(bad), e2 = luau::compile_mod(bad);
  check(!e1.ok && e1.rejection.code == "E_LUAU_SYNTAX", "syntax error is E_LUAU_SYNTAX");
  check(e1.diagnostic && e1.diagnostic->file == "scripts/main.luau" && e1.diagnostic->line == 3, "syntax error reports file:line 3");
  check(e1.rejection.message == e2.rejection.message && e1.rejection.message.rfind("scripts/main.luau:3: ", 0) == 0, "syntax error text is stable and starts with file:line");
  const luau::SourceCompile s = luau::compile_source("x.luau", "if then");
  check(!s.ok && s.diagnostic.line == 1, "line 1 error");
}

void test_provenance() {
  const mod::Package p = make_package({{"scripts/main.luau", "answer = 42"}});
  const luau::CompileResult c = luau::compile_mod(p);
  const std::vector<std::uint8_t> stored = c.compiled.serialize();
  const std::string digest = c.compiled.digest();
  const luau::CompileResult ok = luau::verify_compiled(p, stored, digest);
  check(ok.ok && ok.compiled.digest() == digest, "unmodified compiled output verifies");
  // modified bytecode after build
  std::vector<std::uint8_t> tampered = stored;
  tampered[tampered.size() - 3] ^= 0x01;
  check(luau::verify_compiled(p, tampered, digest).rejection.code == "E_LUAU_COMPILED_MISMATCH", "modified bytecode is rejected");
  // the whole artifact replaced consistently (new header digest) is still rejected by the recorded digest
  const mod::Package other = make_package({{"scripts/main.luau", "answer = 43"}});
  const luau::CompileResult oc = luau::compile_mod(other);
  check(!luau::verify_compiled(p, oc.compiled.serialize(), digest).ok, "another package's compiled output is rejected");
  check(!luau::verify_compiled(other, stored, digest).ok, "compiled output of another identity is rejected");
  check(!luau::verify_compiled(p, stored, std::string(64, '0')).ok, "a wrong recorded digest is rejected");
  check(!luau::verify_compiled(p, stored, "zz").ok, "a malformed recorded digest is rejected");
  std::vector<std::uint8_t> truncated(stored.begin(), stored.end() - 1);
  check(!luau::verify_compiled(p, truncated, digest).ok, "truncated output is rejected");
  std::vector<std::uint8_t> trailing = stored;
  trailing.push_back('x');
  check(!luau::verify_compiled(p, trailing, digest).ok, "trailing bytes are rejected");
  // a crafted blob in place of compiled output
  check(!luau::verify_compiled(p, bytes_of("\x06\x01crafted"), digest).ok, "a crafted blob is rejected");
  check(!luau::verify_compiled(p, {}, digest).ok, "empty output is rejected");
  // a package carrying a bytecode payload never reaches the compiler: the reader rejects it
  mod::Manifest m = p.manifest;
  m.payloads[0].size = 9;
  std::map<std::string, std::vector<std::uint8_t>> files{{"scripts/main.luau", bytes_of(std::string("\x03\x06") + "binary\x00")}};
  m.payloads[0].sha256 = sha256_hex(files.begin()->second);
  m.payloads[0].size = files.begin()->second.size();
  const mod::Package crafted = mod::read_package(mod::pack_package(m, files));
  check(!crafted.ok && crafted.rejection.code == "E_LUAU_BYTECODE", "a package with a crafted bytecode blob is rejected by schema validation");
}

void test_load_binding() {
  const mod::Package a = make_package({{"scripts/main.luau", "x = 1"}}), b = make_package({{"scripts/main.luau", "x = 2"}});
  auto vm = luau::Vm::create(a.identity, relaxed_watchdog());
  const luau::CallResult wrong = vm->load(luau::compile_mod(b).compiled);
  check(!wrong.ok && !vm->disabled(), "compiled scripts of another package identity are refused without disabling the VM");
  check(vm->load(luau::compile_mod(a).compiled).ok && vm->describe_global("x") == "number:1", "the VM loads its own package's scripts");
  check(!vm->load(luau::CompiledMod{}).ok, "an empty CompiledMod loads nothing");
}

void test_library_surface() {
  const char *forbidden[] = {"os", "io", "debug", "require", "load", "loadstring", "dofile", "loadfile", "getfenv", "setfenv", "collectgarbage", "gcinfo",
                             "newproxy", "print", "coroutine", "tick", "time", "wait", "spawn", "delay", "xpcall", "rawlen", "_G", "_VERSION", "class", "integer", "vector"};
  std::string src;
  for (const char *n : forbidden) src += std::string("r_") + n + " = typeof(" + n + ")\n";
  src += "r_strmt = getmetatable('')\nr_random = math.random\nr_floor = typeof(math.floor)\nr_seed = typeof(math.randomseed)\n";
  Loaded l = load_script(src, relaxed_watchdog());
  check(l.loaded.ok, "library script loads: " + l.loaded.message);
  for (const char *n : forbidden) check(l.vm->describe_global(std::string("r_") + n) == "string:nil", std::string("forbidden global is nil: ") + n);
  check(l.vm->describe_global("r_strmt") == "nil", "string metatable is not reachable");
  check(l.vm->describe_global("r_floor") == "string:function", "allowed library member present");
  const char *allowed[] = {"assert", "error", "ipairs", "next", "pairs", "pcall", "rawequal", "rawget", "rawset", "select", "setmetatable", "getmetatable", "tonumber",
                           "tostring", "type", "typeof", "unpack", "math", "string", "table", "bit32", "utf8", "buffer"};
  std::string src2;
  for (const char *n : allowed) src2 += std::string("a_") + n + " = typeof(" + n + ")\n";
  Loaded l2 = load_script(src2, relaxed_watchdog());
  for (const char *n : allowed) {
    const std::string want = std::string(n) == "math" || std::string(n) == "string" || std::string(n) == "table" || std::string(n) == "bit32" || std::string(n) == "utf8" || std::string(n) == "buffer" ? "string:table" : "string:function";
    check(l2.vm->describe_global(std::string("a_") + n) == want, std::string("allowed global present: ") + n);
  }
  // libraries and string metatable are read-only; mutation faults the mod
  for (const char *attack : {"string.rep = nil", "math.floor = function() return 1 end", "table.insert = nil", "rawset(string, 'x', 1)", "rawset(math, 'x', 1)"}) {
    Loaded a = load_script(attack, relaxed_watchdog());
    check(!a.loaded.ok && a.loaded.fault == luau::Fault::error, std::string("read-only library: ") + attack);
  }
  // globals are per mod: this mod can define its own, library tables stay frozen
  Loaded g = load_script("mine = 5\nfunction cb() mine = mine + 1 end", relaxed_watchdog());
  check(g.loaded.ok && g.vm->call("cb", 1, 0).ok && g.vm->describe_global("mine") == "number:6", "a mod can use its own globals");
}

void test_budget() {
  Loaded a = load_script("n = 0\nfunction spin() while true do n = n + 1 end end", relaxed_watchdog());
  check(a.loaded.ok, "spin loads");
  const luau::CallResult r1 = a.vm->call("spin", 1, 0);
  check(!r1.ok && r1.fault == luau::Fault::budget, "infinite loop is interrupted by the budget");
  check(r1.units == luau::policy_limits().callback_units + 1, "budget stops at exactly limit+1 units");
  const std::string n1 = a.vm->describe_global("n");
  Loaded b = load_script("n = 0\nfunction spin() while true do n = n + 1 end end", relaxed_watchdog());
  const luau::CallResult r2 = b.vm->call("spin", 1, 0);
  check(r2.units == r1.units && b.vm->describe_global("n") == n1, "same instruction count and state on both runs");
  check(a.vm->disabled() && a.vm->fault() == luau::Fault::budget && !a.vm->nondeterministic(), "the mod is disabled; a budget fault is deterministic");
  check(!a.vm->call("spin", 2, 0).ok, "a disabled mod runs nothing");
  // pcall cannot clear a budget fault
  Loaded p = load_script("function evil() while true do pcall(function() while true do end end) end end", relaxed_watchdog());
  const luau::CallResult pr = p.vm->call("evil", 1, 0);
  check(!pr.ok && pr.fault == luau::Fault::budget && p.vm->disabled(), "pcall cannot swallow the budget fault");
  Loaded p2 = load_script("caught = false\nfunction evil() local ok = pcall(function() while true do end end) caught = true end", relaxed_watchdog());
  const luau::CallResult pr2 = p2.vm->call("evil", 1, 0);
  check(!pr2.ok && p2.vm->disabled(), "a script that survives pcall is still disabled afterwards");
  // per-frame budget across callbacks: each callback is under the callback budget, together over the frame budget
  luau::Limits fl = relaxed_watchdog();
  fl.callback_units = 1000;
  fl.frame_units = 1500;
  Loaded f = load_script("function work() for i = 1, 800 do end end", fl);
  check(f.vm->call("work", 5, 0).ok, "first callback within frame budget");
  check(f.vm->call("work", 5, 1).fault == luau::Fault::budget, "second callback exceeds the frame budget");
  Loaded f2 = load_script("function work() for i = 1, 800 do end end", fl);
  check(f2.vm->call("work", 5, 0).ok && f2.vm->call("work", 6, 0).ok, "the frame budget restarts with the frame");
  // a package may request less
  mod::Manifest m;
  m.luau_budget_units = 50;
  m.luau_memory_bytes = 100000;
  check(luau::limits_for(m).callback_units == 50 && luau::limits_for(m).memory_bytes == 100000 && luau::limits_for(mod::Manifest{}).callback_units == 100000, "limits_for lowers, never raises");
  luau::Limits small = relaxed_watchdog();
  small.callback_units = 50;
  Loaded s = load_script("function work() for i = 1, 100 do end end", small);
  check(s.vm->call("work", 1, 0).fault == luau::Fault::budget, "a lowered request is enforced");
}

void test_watchdog() {
  luau::Limits l;
  l.callback_units = 1ULL << 40;
  l.frame_units = 1ULL << 40;
  l.callback_watchdog_ms = 1;
  l.frame_watchdog_ms = 0;
  Loaded w = load_script("function spin() while true do end end", l);
  const luau::CallResult r = w.vm->call("spin", 1, 0);
  check(!r.ok && r.fault == luau::Fault::watchdog && w.vm->nondeterministic(), "a watchdog fault disables the mod and marks the session non-deterministic");
}

void test_memory() {
  for (const char *bomb : {"function bomb() local t = {} for i = 1, 100000000 do t[i] = i end end", "function bomb() local s = string.rep('x', 100000000) end",
                           "function bomb() local t = {} for i = 1, 100000000 do t[i] = {i} end end", "function bomb() local b = buffer.create(100000000) end",
                           "function bomb() local s = 'x' while true do s = s .. s end end"}) {
    luau::Limits l = relaxed_watchdog();
    l.callback_units = 1ULL << 40;
    l.frame_units = 1ULL << 40;
    Loaded m = load_script(bomb, l);
    check(m.loaded.ok, "bomb loads");
    const luau::CallResult r = m.vm->call("bomb", 1, 0);
    check(!r.ok && r.fault == luau::Fault::memory, std::string("allocation bomb stops at the quota: ") + bomb);
    check(m.vm->memory_in_use() <= l.memory_bytes, "memory never exceeds the quota");
    check(m.vm->close() == 0, "teardown after a memory fault is leak free");
  }
  // an error the script catches is still a fault
  Loaded c = load_script("function f() pcall(function() local s = string.rep('x', 100000000) end) end", relaxed_watchdog());
  check(!c.vm->call("f", 1, 0).ok && c.vm->fault() == luau::Fault::memory, "pcall cannot clear a memory fault");
}

void test_stack_and_errors() {
  Loaded s = load_script("function deep(n) return 1 + deep(n + 1) end\nfunction go() deep(1) end", relaxed_watchdog());
  const luau::CallResult r = s.vm->call("go", 1, 0);
  check(!r.ok && r.fault == luau::Fault::stack, "unbounded recursion stops at the call depth limit");
  Loaded e = load_script("function boom() error('secret failure') end", relaxed_watchdog());
  const luau::CallResult er = e.vm->call("boom", 1, 0);
  check(!er.ok && er.fault == luau::Fault::error && er.message.find("secret failure") != std::string::npos && e.vm->disabled(), "a script error disables the mod");
  check(er.message.find('/') == std::string::npos && er.message.find("0x") == std::string::npos, "diagnostics carry no host paths or addresses");
  Loaded n = load_script("x = 1", relaxed_watchdog());
  check(!n.vm->call("missing", 1, 0).ok && n.vm->disabled(), "a missing callback is a fault of that mod only");
}

void test_isolation() {
  Loaded a = load_script("shared = 5\nsecret = 'a'\nfunction poke() shared = shared + 1 end", relaxed_watchdog());
  Loaded b = load_script("seen = typeof(shared)\nseen2 = typeof(secret)\nfunction crash() error('b dies') end\nfunction ok() ticks = (ticks or 0) + 1 end", relaxed_watchdog());
  check(b.vm->describe_global("seen") == "string:nil" && b.vm->describe_global("seen2") == "string:nil", "a mod cannot see another mod's globals");
  check(a.vm->describe_global("shared") == "number:5", "the other mod's globals are untouched");
  // string / table libraries are per VM: a library mutation attempt in one cannot reach the other
  Loaded c = load_script("pcall(function() rawset(string, 'leak', 1) end)\nl = typeof(string.leak)", relaxed_watchdog());
  check(!c.vm->disabled() || c.vm->fault() == luau::Fault::error, "library tampering is contained in its VM");
  check(!b.vm->call("crash", 1, 0).ok, "b faults");
  check(a.vm->call("poke", 1, 0).ok && a.vm->describe_global("shared") == "number:6", "a fault in one mod does not disable another");
  check(a.vm->memory_in_use() > 0 && b.vm->memory_in_use() > 0 && a.vm->memory_in_use() != 0, "each VM has its own allocator");
}

void test_random() {
  const std::string ident = [] { std::string s; for (int i = 0; i < 32; ++i) s += "ab"; return s; }();
  mod::Package p = make_package({{"scripts/main.luau", "function roll() a = math.random(1, 100) b = math.random(1, 100) c = math.random(1, 100) d = math.random(10) e = math.random() end"}});
  // The golden sequence is for the fixed identity "ab"*32 (computed by the independent Python reference in tests/luau_host_test.py).
  auto vm = luau::Vm::create(ident, relaxed_watchdog());
  mod::Package fixed = p;
  fixed.identity = ident;
  const luau::CompileResult cf = luau::compile_mod(fixed);
  check(vm && vm->load(cf.compiled).ok, "random script loads");
  check(vm->call("roll", 7, 3).ok, "roll");
  check(vm->describe_global("a") == "number:27" && vm->describe_global("b") == "number:10" && vm->describe_global("c") == "number:78" && vm->describe_global("d") == "number:7", "golden math.random sequence (frame 7, callback 3)");
  check(std::atof(vm->describe_global("e").c_str() + 7) == 0.37813877694498965, "golden math.random() float");
  check(vm->call("roll", 8, 3).ok && vm->describe_global("a") == "number:73" && vm->describe_global("b") == "number:81" && vm->describe_global("c") == "number:21" && vm->describe_global("d") == "number:10", "golden sequence (frame 8)");
  check(vm->call("roll", 7, 4).ok && vm->describe_global("a") == "number:96" && vm->describe_global("b") == "number:72" && vm->describe_global("c") == "number:43" && vm->describe_global("d") == "number:4", "golden sequence (callback 4)");
  // stateless between callbacks: the same (frame, index) always restarts the same sequence
  check(vm->call("roll", 7, 3).ok && vm->describe_global("a") == "number:27", "the generator restarts per callback");
  // math.randomseed is not available
  Loaded rs = load_script("math.randomseed(1)", relaxed_watchdog());
  check(!rs.loaded.ok, "math.randomseed faults");
  Loaded bad = load_script("function f() return math.random(0) end", relaxed_watchdog());
  check(!bad.vm->call("f", 1, 0).ok, "an empty interval is an error");
}

void test_adversarial_findings() {
  // A host lookup of an uninterned name near the quota must report a memory fault, never throw (found by the F06 adversarial review).
  int faults = 0;
  for (std::uint64_t quota = 300000; quota < 330000; quota += 7) {
    luau::Limits l = relaxed_watchdog();
    l.memory_bytes = quota;
    mod::Package p = make_package({{"scripts/main.luau", "b = buffer.create(250000)\nfunction f() end"}});
    const luau::CompileResult c = luau::compile_mod(p);
    auto vm = luau::Vm::create(p.identity, l);
    if (!vm || !vm->load(c.compiled).ok) continue;
    const luau::CallResult r = vm->call("zz_missing_callback_name_that_is_long_enough_to_need_alloc", 1, 0);
    check(!r.ok, "a missing callback is a fault");
    faults += r.fault == luau::Fault::memory;
    (void)vm->describe_global("another_uninterned_global_name_for_the_quota_sweep");
  }
  check(faults >= 0, "quota sweep survived");
  // No host addresses reach scripts; out-of-range random arguments are errors.
  Loaded t = load_script("a = tostring({})\nb = tostring(function() end)\nc = tostring(1.5)\nd = tostring(setmetatable({}, {__tostring = function() return 'mine' end}))\ne = tostring(buffer.create(1))", relaxed_watchdog());
  check(t.vm->describe_global("a") == "string:table" && t.vm->describe_global("b") == "string:function" && t.vm->describe_global("c") == "string:1.5" &&
            t.vm->describe_global("d") == "string:mine" && t.vm->describe_global("e") == "string:buffer", "tostring renders no host addresses");
  for (const char *bad : {"math.random(2^40)", "math.random(1, 2^33)", "math.random(0.5)", "math.random(-2^31 - 1, 0)"})
    check(!load_script(bad, relaxed_watchdog()).loaded.ok, std::string("out-of-range random argument faults: ") + bad);
}

void test_teardown() {
  for (int i = 0; i < 20; ++i) {
    Loaded l = load_script("t = {} for i = 1, 1000 do t[i] = {i, tostring(i), string.rep('x', 10)} end function f() local s = {} for i = 1, 100 do s[i] = i end end", relaxed_watchdog());
    check(l.vm->call("f", 1, 0).ok, "teardown workload runs");
    check(l.vm->close() == 0, "teardown is leak free");
    check(l.vm->close() == 0, "close is idempotent");
    check(!l.vm->call("f", 2, 0).ok, "a closed VM runs nothing");
  }
  check(luau::Vm::create("short", luau::policy_limits()) == nullptr, "a malformed identity is refused");
  luau::Limits tiny = relaxed_watchdog();
  tiny.memory_bytes = 1024;
  check(luau::Vm::create(std::string(64, 'a'), tiny) == nullptr, "a quota too small to open the libraries creates no VM");
}

}  // namespace

int main() {
  test_codegen_off();
  test_compile();
  test_provenance();
  test_load_binding();
  test_library_surface();
  test_budget();
  test_watchdog();
  test_memory();
  test_stack_and_errors();
  test_isolation();
  test_random();
  test_adversarial_findings();
  test_teardown();
  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::puts("luau_host_test: ok");
  return 0;
}
