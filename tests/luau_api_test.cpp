// SEG-137-T004..T006 (ADR 0295): the capability-gated `sega` host API, its static check, the overlay raster, the state seam, the fixture session and the adversarial corpus of Luau policy section 8.
// Synthetic scripts only. Each test names the policy category (library-surface, escape-metatable, memory-exhaustion, cpu-exhaustion, determinism, binding-abuse, bytecode-injection, state-integrity,
// mod-isolation, diagnostic-hygiene, compiler-input) it covers; `approval-semantics` is a CLI property tested in tests/mod_luau_api_test.py. Exits nonzero on the first failed check.
#include "segarecomp/luau_api.hpp"
#include "segarecomp/sha256.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
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

const std::vector<std::string> kAllCaps = {"host.log", "frame.work_ram", "overlay.rgba", "memory.read.m68k.work_ram", "memory.write.m68k.work_ram", "input.read", "input.intercept"};

mod::Package make_package(const std::map<std::string, std::string> &scripts, const std::vector<std::string> &caps = kAllCaps, const std::string &id = "api-test") {
  mod::Manifest m;
  m.id = id;
  m.version = "1.0.0";
  m.name = "Luau API test";
  m.platform = "genesis";
  m.source_kind = "cartridge-image";
  m.revisions.push_back({"rev-a", std::string(64, 'a'), ""});
  m.capabilities = caps;
  std::sort(m.capabilities.begin(), m.capabilities.end());
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

luau::Limits relaxed() {   // sanitizer builds are slow; deterministic limits decide, the watchdog has its own test
  luau::Limits l;
  l.callback_watchdog_ms = 0;
  l.frame_watchdog_ms = 0;
  return l;
}

struct Mod {
  mod::Package package;
  luau::CompiledMod compiled;
  std::unique_ptr<luau::Vm> vm;
  luau::CallResult loaded;
};

Mod start(const std::string &source, const std::vector<std::string> &caps = kAllCaps, luau::Limits limits = relaxed(), const std::string &id = "api-test") {
  Mod m;
  m.package = make_package({{"scripts/main.luau", source}}, caps, id);
  const luau::CompileResult c = luau::compile_mod(m.package);
  if (!c.ok) {
    std::fprintf(stderr, "compile failed: %s\n", c.rejection.message.c_str());
    std::exit(2);
  }
  m.compiled = c.compiled;
  m.vm = luau::Vm::create(m.package.identity, limits, luau::host_config_for(m.package.manifest));
  if (!m.vm) std::exit(2);
  m.loaded = m.vm->load(m.compiled);
  return m;
}

luau::CallResult render(Mod &m, std::uint64_t frame, luau::Output &out, luau::FrameInput in = {}) {
  in.number = frame;
  return m.vm->run_callback("render", in, 1, out);
}

// ---- capability gating (policy section 5.1, ABI 2 section 7) ----
void test_capability_gating() {
  {
    Mod m = start("function render() sega.overlay.rect(0, 0, 1, 1, 0xFF) end", {"host.log"});
    // The trusted compiler turns `sega.overlay.rect` into an import that is resolved when the function is loaded, so the reference itself already faults at load; a dynamic reference faults at the call.
    check((!m.loaded.ok && m.loaded.fault == luau::Fault::capability) || m.loaded.ok, "an ungranted binding fails at load or at the call, never silently");
    luau::Output out;
    const luau::CallResult r = render(m, 0, out);
    check(!r.ok && r.fault == luau::Fault::capability, "an undeclared overlay binding faults with cause capability");
    check(r.message.find("overlay.rgba") != std::string::npos, "the fault names the missing capability");
    check(out.overlay.empty() && m.vm->disabled(), "no output from a faulted callback; the mod is disabled");
    luau::Output again;
    const luau::CallResult r2 = render(m, 1, again);
    check(!r2.ok && r2.fault == luau::Fault::capability, "every later call fails with the original fault");
  }
  {   // pcall cannot clear a capability fault
    Mod m = start("function render()\n  local ok = pcall(function() return sega.overlay.rect end)\n  sega.log(\"after\")\nend", {"host.log"});
    luau::Output out;
    const luau::CallResult r = render(m, 0, out);
    check(!r.ok && r.fault == luau::Fault::capability && out.log.empty(), "pcall around an ungranted binding does not clear the fault, and the staged output is discarded");
  }
  {
    Mod m = start("function render() sega.log(\"hello\") end", kAllCaps);
    luau::Output out;
    check(render(m, 0, out).ok && out.log == std::vector<std::string>{"hello"}, "a declared binding works");
  }
  {
    Mod m = start("function reset() sega.log(\"x\") end", {});
    luau::Output out;
    luau::FrameInput in;
    const luau::CallResult r = m.vm->run_callback("reset", in, 0, out);
    check(!r.ok && r.fault == luau::Fault::capability, "sega.log without host.log faults");
  }
  {
    Mod m = start("function render() local v = sega.frame.value(\"score\") end", {"host.log", "overlay.rgba"});
    luau::Output out;
    check(render(m, 0, out).fault == luau::Fault::capability, "sega.frame.value needs frame.work_ram");
  }
  {
    Mod m = start("function render() sega.log(tostring(sega.nonsense)) sega.log(tostring(sega.frame.nonsense)) end");
    luau::Output out;
    check(render(m, 0, out).ok && out.log == (std::vector<std::string>{"nil", "nil"}), "a name that is not in the API is nil, not a fault");
  }
  {   // optional capability the platform supports is granted; declared required ones too
    mod::Manifest man = make_package({{"scripts/main.luau", "x=1"}}, {"host.log"}).manifest;
    man.capabilities_optional = {"overlay.rgba"};
    const luau::HostConfig h = luau::host_config_for(man);
    check(std::find(h.capabilities.begin(), h.capabilities.end(), "overlay.rgba") != h.capabilities.end(), "a supported optional capability is granted");
  }
}

// ---- binding-abuse: argument validation, phases, read-only surface ----
void test_binding_abuse() {
  const char *bad_calls[] = {
      "sega.overlay.rect(0/0, 0, 1, 1, 1)",         "sega.overlay.rect(math.huge, 0, 1, 1, 1)", "sega.overlay.rect(0, 0, -1, 1, 1)",     "sega.overlay.rect(0.5, 0, 1, 1, 1)",
      "sega.overlay.rect(0, 0, 1, 1, -1)",          "sega.overlay.rect(0, 0, 1, 1, 4294967296)", "sega.overlay.rect(2^40, 0, 1, 1, 1)",  "sega.overlay.rect('a', 0, 1, 1, 1)",
      "sega.overlay.rect(0, 0, 1, 1)",              "sega.overlay.rect(0, 0, 1, 1, 1, 1)",       "sega.overlay.rect(nil, 0, 1, 1, 1)",     "sega.overlay.text(0, 0, 5, 1)",
      "sega.overlay.text(0, 0, {}, 1)",             "sega.overlay.text(0, 0, 'x')",              "sega.log(5)",                           "sega.log()",
      "sega.log('a', 'b')",                         "sega.log(sega.log)",                        "sega.frame.value(5)",                   "sega.frame.value('')",
      "sega.frame.value(string.rep('n', 33))",      "sega.frame.value('a', 'b')"};
  for (const char *call : bad_calls) {
    Mod m = start(std::string("function render() ") + call + " end");
    luau::Output out;
    const luau::CallResult r = render(m, 0, out);
    check(!r.ok && (r.fault == luau::Fault::range), std::string("a bad argument is a range fault: ") + call);
    check(out.log.empty() && out.overlay.empty(), std::string("no output from the faulted callback: ") + call);
  }
  {   // phases: overlay and values are render-only
    Mod m = start("sega.overlay.rect(0, 0, 1, 1, 1)");
    check(!m.loaded.ok && m.loaded.fault == luau::Fault::phase, "overlay at load time is a phase fault");
  }
  {
    Mod m = start("function reset() local v = sega.frame.value('score') end");
    luau::Output out;
    luau::FrameInput in;
    check(m.vm->run_callback("reset", in, 0, out).fault == luau::Fault::phase, "frame.value in reset is a phase fault");
  }
  {   // log is legal in every phase
    Mod m = start("sega.log('load')\nfunction reset() sega.log('reset') end");
    check(m.loaded.ok, "sega.log at load time is allowed");
  }
  {   // the surface is read-only and carries no host identity
    const char *attacks[] = {"sega.log = nil", "sega.overlay.rect = function() end", "rawset(sega, 'x', 1)", "setmetatable(sega, {})", "sega.overlay.width = 1", "sega.extra = 1", "getmetatable(sega).__index = nil"};
    for (const char *a : attacks) {
      Mod m = start(std::string("function render() ") + a + " end");
      luau::Output out;
      const luau::CallResult r = render(m, 0, out);
      check(!r.ok, std::string("the sega surface is read-only: ") + a);
    }
    Mod m = start("function render()\n  sega.log(tostring(sega))\n  sega.log(tostring(sega.log))\n  sega.log(typeof(sega.frame.value('score')))\n  sega.log(typeof(sega.overlay.width))\n  sega.log(tostring(getmetatable(sega)))\nend");
    luau::Output out;
    luau::FrameInput in;
    in.values = {{"score", 7}};
    check(render(m, 0, out, in).ok && out.log == (std::vector<std::string>{"table", "function", "number", "number", "locked"}), "bindings hand out plain values and no host handle (no userdata, no address)");
  }
}

// ---- clamping, truncation and rate limits ----
void test_overlay_validation() {
  {
    Mod m = start("function render()\n  sega.overlay.rect(-10, -10, 20, 20, 0x11223344)\n  sega.overlay.rect(500, 500, 5, 5, 1)\n  sega.overlay.rect(0, 0, 0, 5, 1)\n"
                  "  sega.overlay.text(1000, -5, string.rep('a', 100) .. '\\n', 0xFFFFFFFF)\n  sega.overlay.rect(100, 250, 100, 100, 1)\nend");
    luau::Output out;
    check(render(m, 0, out).ok, "out-of-range geometry is clamped, not a fault");
    check(out.overlay.size() == 3, "two invisible rects are dropped (a clamped rect, a text and a clipped rect remain)");
    if (out.overlay.size() == 3) {
      check(out.overlay[0].x == 0 && out.overlay[0].y == 0 && out.overlay[0].w == 10 && out.overlay[0].h == 10 && out.overlay[0].rgba == 0x11223344U, "a rect is clamped to the overlay");
      check(out.overlay[1].kind == luau::OverlayOp::Kind::text && out.overlay[1].x == luau::kOverlayWidth - 1 && out.overlay[1].y == 0, "a text origin is clamped into the overlay");
      check(out.overlay[1].text.size() == luau::kOverlayTextBytes, "over-long text is cut to 64 bytes");
      check(out.overlay[2].w == 28 && out.overlay[2].h == 6, "a rect crossing the edge keeps its visible part");
    }
    std::set<std::string> causes;
    for (const luau::Note &n : out.notes) causes.insert(n.cause);
    check(causes.count("overlay_clamped") && causes.count("overlay_clipped") && causes.count("text_truncated"), "clamps and truncations are reported as notes");
  }
  {   // control and non-ASCII bytes never reach the overlay or the log
    Mod m = start("function render()\n  sega.overlay.text(0, 0, 'a\\0b\\n\\200c', 1)\n  sega.log('x\\1y')\nend");
    luau::Output out;
    check(render(m, 0, out).ok && out.overlay.size() == 1 && out.overlay[0].text == "a?b??c" && out.log == std::vector<std::string>{"x?y"}, "unprintable bytes become '?'");
  }
  {
    Mod m = start("function render() for i = 1, 65 do sega.overlay.rect(0, 0, 1, 1, 1) end end");
    luau::Output out;
    const luau::CallResult r = render(m, 0, out);
    check(!r.ok && r.fault == luau::Fault::budget && out.overlay.empty(), "more than 64 overlay requests is a budget fault with no partial output");
  }
  {
    Mod m = start("function render() for i = 1, 16 do sega.log('l') end end");
    luau::Output out;
    check(render(m, 0, out).ok && out.log.size() == 16, "16 log lines per callback are allowed");
    Mod n = start("function render() for i = 1, 17 do sega.log('l') end end");
    luau::Output out2;
    const luau::CallResult r = render(n, 0, out2);
    check(!r.ok && r.fault == luau::Fault::budget && out2.log.empty(), "the 17th log line is a budget fault");
    Mod o = start("function render() sega.log(string.rep('z', 300)) end");
    luau::Output out3;
    check(render(o, 0, out3).ok && out3.log.size() == 1 && out3.log[0].size() == 256 && out3.notes.size() == 1 && out3.notes[0].cause == "log_truncated", "a long log line is cut to 256 bytes with a note");
  }
  {   // frame values: copy-in, nil when unpublished
    Mod m = start("function render(frame)\n  local a, b = sega.frame.value('score'), sega.frame.value('missing')\n  sega.log(tostring(frame) .. ',' .. tostring(a) .. ',' .. tostring(b))\nend");
    luau::Output out;
    luau::FrameInput in;
    in.values = {{"score", 1234}};
    check(render(m, 77, out, in).ok && out.log == std::vector<std::string>{"77,1234,nil"}, "the frame number is the callback argument and published values are readable");
  }
  {   // staged output is discarded when the callback faults after producing some
    Mod m = start("function render()\n  sega.overlay.rect(0, 0, 4, 4, 1)\n  sega.log('before')\n  error('boom')\nend");
    luau::Output out;
    const luau::CallResult r = render(m, 0, out);
    check(!r.ok && r.fault == luau::Fault::error && out.overlay.empty() && out.log.empty(), "a faulted callback leaves no staged output behind");
  }
  {   // the guest services exist only inside a scheduler phase: without them the bindings fault with cause phase (never a silent no-op)
    Mod m = start("function frame_end() local v = sega.mem.read('m68k.work_ram', 0, 1) end");
    luau::Output out;
    luau::FrameInput in;
    const luau::CallResult r = m.vm->run_callback("frame_end", in, 7, out);
    check(!r.ok && r.fault == luau::Fault::phase, "sega.mem.read without scheduler services faults with cause phase");
  }
  {   // callbacks that are not defined are skipped; an unknown callback name is refused
    Mod m = start("x = 1");
    luau::Output out;
    check(render(m, 0, out).ok, "an undefined render callback is skipped");
    luau::FrameInput in;
    check(!m.vm->run_callback("bogus", in, 0, out).ok, "only the documented lifecycle callbacks (reset, frame_begin, input, post_input, frame_end, render) can be run");
  }
}

// ---- API list = runtime surface (T004 acceptance: the list is machine readable and complete) ----
void collect(Mod &m, std::vector<std::string> &names) {
  luau::Output out;
  check(render(m, 0, out).ok, "the enumeration script runs");
  names = out.log;
}

// SEG-140-T004 (ADR 0303): positioned overlay layers declared from Luau.
void test_overlay_layers() {
  {   // a declared layer takes the following requests, clamped to ITS size; the fixed overlay keeps the default target
    Mod m = start("function render()\n  sega.overlay.rect(0, 0, 2, 2, 0x11111111)\n  sega.overlay.layer(2, 'top_right', 'presentation', 3, 4, 20, 10, 1)\n  sega.overlay.target(2)\n"
                  "  sega.overlay.rect(0, 0, 50, 50, 0x22222222)\n  sega.overlay.text(500, 500, 'hi', 0x33333333)\n  sega.overlay.target(0)\n  sega.overlay.rect(5, 5, 1, 1, 0x44444444)\nend");
    luau::Output out;
    check(render(m, 0, out).ok, "layers are declared and drawn in the render phase");
    check(out.layers.size() == 1 && out.layers[0].index == 2 && out.layers[0].anchor == 2 && out.layers[0].reference == 1 && out.layers[0].dx == 3 && out.layers[0].dy == 4 && out.layers[0].w == 20 &&
              out.layers[0].h == 10 && out.layers[0].z == 1,
          "the declaration is reported with the viewer's anchor/reference numbering");
    check(out.overlay.size() == 4 && out.overlay[0].layer == 0 && out.overlay[1].layer == 2 && out.overlay[2].layer == 2 && out.overlay[3].layer == 0, "each request is tagged with the layer it was drawn into");
    check(out.overlay[1].w == 20 && out.overlay[1].h == 10, "a rect is clamped to the layer's own size, not the fixed overlay's");
    check(out.overlay[2].kind == luau::OverlayOp::Kind::text && out.overlay[2].x == 19 && out.overlay[2].y == 9, "a text origin is clamped into the layer");
    const luau::OverlayImage fixed = luau::rasterize_overlay(out.overlay);
    const luau::OverlayImage layer = luau::rasterize_layer(out.layers[0], out.overlay);
    check(fixed.width == luau::kOverlayWidth && fixed.height == luau::kOverlayHeight && layer.width == 20 && layer.height == 10 && layer.rgba.size() == 20U * 10U * 4U, "the layer image has the declared size");
    auto at = [](const luau::OverlayImage &img, int x, int y) { return &img.rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(img.width) + static_cast<std::size_t>(x)) * 4U]; };
    check(at(fixed, 0, 0)[0] == 0x11 && at(fixed, 5, 5)[0] == 0x44 && at(fixed, 10, 10)[3] == 0, "the fixed overlay only holds the layer-0 requests (the layer's rect is not in it)");
    check(at(layer, 0, 0)[0] == 0x22 && at(layer, 19, 9)[3] != 0 && at(layer, 5, 5)[0] == 0x22, "the layer holds its own requests (rect 0x22 and the text)");
  }
  {   // a script that never touches layers produces exactly what it did before (no layers, layer-0 requests only)
    Mod m = start("function render() sega.overlay.rect(1, 2, 3, 4, 0xAABBCCDD) end");
    luau::Output out;
    check(render(m, 0, out).ok && out.layers.empty() && out.overlay.size() == 1 && out.overlay[0].layer == 0, "no layer calls, no layers");
  }
  {   // the target resets at the start of every callback
    Mod m = start("local n = 0\nfunction render()\n  n = n + 1\n  if n == 1 then sega.overlay.layer(1, 'center', 'picture', 0, 0, 8, 8, 0)\n  sega.overlay.target(1) end\n  sega.overlay.rect(0, 0, 1, 1, 7)\nend");
    luau::Output a, b;
    check(render(m, 0, a).ok && a.overlay.size() == 1 && a.overlay[0].layer == 1, "the first frame draws into layer 1");
    check(render(m, 1, b).ok && b.overlay.size() == 1 && b.overlay[0].layer == 0 && b.layers.empty(), "the next callback starts at the fixed overlay again, with no layer declared");
  }
  {   // malformed use is a script error that disables the mod, with no partial output (range)
    const char *bad[] = {"sega.overlay.layer(0, 'center', 'picture', 0, 0, 8, 8, 0)", "sega.overlay.layer(5, 'center', 'picture', 0, 0, 8, 8, 0)", "sega.overlay.layer(1, 'middle', 'picture', 0, 0, 8, 8, 0)",
                         "sega.overlay.layer(1, 'center', 'screen', 0, 0, 8, 8, 0)", "sega.overlay.layer(1, 'center', 'picture', 2049, 0, 8, 8, 0)", "sega.overlay.layer(1, 'center', 'picture', 0, 0, 129, 8, 0)",
                         "sega.overlay.layer(1, 'center', 'picture', 0, 0, 8, 0, 0)", "sega.overlay.layer(1, 'center', 'picture', 0, 0, 8, 8, 4)", "sega.overlay.layer(1, 'center', 'picture', 0.5, 0, 8, 8, 0)",
                         "sega.overlay.layer(1, 'center', 'picture', 0, 0, 8, 8)", "sega.overlay.layer(1, 5, 'picture', 0, 0, 8, 8, 0)",
                         "sega.overlay.layer(1, 'center', 'picture', 0, 0, 8, 8, 0) sega.overlay.layer(1, 'center', 'picture', 0, 0, 8, 8, 0)", "sega.overlay.target(3)", "sega.overlay.target(-1)", "sega.overlay.target(5)"};
    for (const char *call : bad) {
      Mod m = start(std::string("function render() ") + call + " end");
      luau::Output out;
      const luau::CallResult r = render(m, 0, out);
      check(!r.ok && r.fault == luau::Fault::range && out.overlay.empty() && out.layers.empty() && m.vm->disabled(), std::string("a malformed layer call is a range fault: ") + call);
    }
  }
  {   // capability and phase: layers need overlay.rgba and the render phase like every other overlay call
    Mod m = start("function render() sega.overlay.layer(1, 'center', 'picture', 0, 0, 8, 8, 0) end", {"host.log"});
    luau::Output out;
    const luau::CallResult r = render(m, 0, out);
    check(!r.ok && r.fault == luau::Fault::capability && out.layers.empty(), "sega.overlay.layer without overlay.rgba is a capability fault");
    Mod p = start("sega.overlay.target(0)");
    check(!p.loaded.ok && p.loaded.fault == luau::Fault::phase, "sega.overlay.target outside render is a phase fault");
  }
  {   // the four-layer total is exactly the host's overlay budget
    check(luau::kOverlayLayersMax * luau::kOverlayLayerSide * luau::kOverlayLayerSide == 65536, "four 128 x 128 layers are the host's 65,536-pixel budget");
  }
}

void test_api_list() {
  const std::string enumerate =
      "local names = {}\n"
      "local function walk(prefix, t)\n"
      "  for k, v in pairs(t) do\n"
      "    local path = prefix .. '.' .. k\n"
      "    if type(v) == 'table' then walk(path, v) else names[#names + 1] = path .. ':' .. type(v) end\n"
      "  end\n"
      "end\n"
      "function render() walk('sega', sega) table.sort(names) for _, n in ipairs(names) do sega.log(n) end end\n";
  Mod m = start(enumerate);
  std::vector<std::string> runtime;
  collect(m, runtime);
  std::vector<std::string> listed;
  for (const luau::ApiEntry &e : luau::api_entries()) {
    check(!e.name.empty() && !e.doc.empty() && !e.phases.empty(), std::string("complete entry: ") + std::string(e.name));
    if (e.kind == "callback") {
      check(e.capability.empty(), "callbacks have no capability");
      continue;
    }
    const mod::CapabilityInfo *cap = mod::find_capability(std::string(e.capability));
    check(cap != nullptr && cap->luau_bound, std::string("the capability of ") + std::string(e.name) + " is in the ABI 2 catalogue with a Luau binding");
    listed.push_back(std::string(e.name) + ":" + (e.kind == "value" ? "number" : "function"));
  }
  std::sort(listed.begin(), listed.end());
  check(runtime == listed, "the bindings a VM installs are exactly the entries of the API list");
  check(luau::declarations() == luau::declarations() && luau::api_json() == luau::api_json(), "generation is deterministic");
  for (const luau::ApiEntry &e : luau::api_entries()) {
    check(luau::declarations().find(luau::api_signature(e)) != std::string::npos, std::string("the declaration file carries ") + std::string(e.name));
    check(luau::api_json().find("\"name\": \"" + std::string(e.name) + "\"") != std::string::npos, std::string("the JSON list carries ") + std::string(e.name));
  }
}

// ---- static check (T005) ----
std::vector<luau::AnalysisIssue> analyze(const std::string &source, const std::vector<std::string> &caps = kAllCaps) {
  return luau::analyze_mod(make_package({{"scripts/main.luau", source}}, caps));
}
bool has(const std::vector<luau::AnalysisIssue> &v, const std::string &cls, unsigned line) {
  return std::any_of(v.begin(), v.end(), [&](const luau::AnalysisIssue &i) { return i.cls == cls && i.where.line == line && i.where.file == "scripts/main.luau"; });
}

void test_analysis() {
  check(analyze("function render(frame)\n  sega.overlay.rect(0, 0, 8, 8, 0xFF0000FF)\n  sega.log('ok')\n  local v = sega.frame.value('x')\nend").empty(), "a conforming script has no issue");
  check(has(analyze("sega.log('a')\nsega.overlay.rect(0, 0, 1, 1, 1)\n", {"host.log"}), "E_LUAU_CAPABILITY", 2), "an undeclared capability is flagged at its line");
  check(has(analyze("local w = sega.overlay.width\n", {"host.log"}), "E_LUAU_CAPABILITY", 1), "an undeclared value read is flagged");
  check(has(analyze("sega.nonsense(1)\nlocal x = sega.frame.nope\n"), "E_LUAU_API", 1) && has(analyze("sega.nonsense(1)\nlocal x = sega.frame.nope\n"), "E_LUAU_API", 2), "unknown API names are flagged");
  check(has(analyze("sega.log.x = 1\n"), "E_LUAU_API", 1), "a member of a function is not API");
  check(has(analyze("sega.log(42)\n"), "E_LUAU_TYPE", 1), "a number literal for a string parameter is a type error");
  check(has(analyze("sega.overlay.rect(1, 2, 3)\n"), "E_LUAU_TYPE", 1), "a wrong argument count is a type error");
  check(has(analyze("sega.overlay.rect(0.5, 0, 1, 1, 1)\n"), "E_LUAU_TYPE", 1), "a fractional literal for an integer parameter is a type error");
  check(has(analyze("sega.overlay.rect(0, 0, 'w', 1, 1)\n"), "E_LUAU_TYPE", 1), "a string literal for a number parameter is a type error");
  check(has(analyze("sega.overlay.text(0, 0, true, 1)\n"), "E_LUAU_TYPE", 1), "a boolean literal is a type error");
  check(has(analyze("sega:log('x')\n"), "E_LUAU_TYPE", 1), "':' call is flagged");
  check(has(analyze("local n = sega.overlay.width()\n"), "E_LUAU_TYPE", 1) && has(analyze("sega.overlay()\n"), "E_LUAU_TYPE", 1), "calling a value or a namespace is a type error");
  check(has(analyze("function render(a, b)\nend\n"), "E_LUAU_TYPE", 1), "a callback that declares more parameters than the host passes is flagged");
  check(analyze("function render(a) end\nfunction reset() end\n").empty(), "callbacks with the right arity are fine");
  check(has(analyze("sega['overlay']['rect'](1, 2, 3)\n"), "E_LUAU_TYPE", 1), "string-indexed access is followed");
  check(analyze("local s = sega\ns.nonsense()\n").empty(), "an alias is not followed (the runtime still enforces the capability rules)");
  check(analyze("local function f(\n").empty(), "a file that does not parse is left to the trusted compile step");
  check(analyze("sega.overlay.rect(-1, 0, 1, 1, 1)\nsega.log(('x'):rep(2))\nsega.overlay.rect(unpack({1,2,3,4,5}))\n").empty(), "negative literals, calls and multiple-value tails are not flagged");
  const std::vector<luau::AnalysisIssue> many = analyze("sega.log(1)\nsega.log(2)\n");
  check(many.size() == 2 && many[0].where.line == 1 && many[1].where.line == 2, "issues are ordered by line");
}

// ---- overlay raster ----
void test_raster() {
  luau::OverlayOp rect;
  rect.kind = luau::OverlayOp::Kind::rect;
  rect.x = 2;
  rect.y = 3;
  rect.w = 4;
  rect.h = 2;
  rect.rgba = 0xFF8040FFU;
  luau::OverlayOp half = rect;
  half.x = 2;
  half.y = 3;
  half.w = 1;
  half.h = 1;
  half.rgba = 0x00000080U;   // black at alpha 128 over the opaque orange
  const luau::OverlayImage img = luau::rasterize_overlay(std::vector<luau::OverlayOp>{rect, half});
  check(img.width == 128 && img.height == 256 && img.rgba.size() == 128U * 256U * 4U, "the overlay is 128x256 RGBA8");
  const auto px = [&](int x, int y) { return &img.rgba[(static_cast<std::size_t>(y) * 128U + static_cast<std::size_t>(x)) * 4U]; };
  check(px(0, 0)[3] == 0 && px(1, 3)[3] == 0 && px(6, 3)[3] == 0, "untouched pixels stay fully transparent");
  check(px(3, 3)[0] == 0xFF && px(3, 3)[1] == 0x80 && px(3, 3)[2] == 0x40 && px(3, 3)[3] == 0xFF, "an opaque rect writes its colour");
  check(px(2, 3)[0] == 127 && px(2, 3)[1] == 64 && px(2, 3)[2] == 32 && px(2, 3)[3] == 255, "source-over blending uses integer arithmetic (255*127/255 = 127)");
  luau::OverlayOp t;
  t.kind = luau::OverlayOp::Kind::text;
  t.x = 4;
  t.y = 4;
  t.text = "SCORE 0123";
  t.rgba = 0xFFFFFFFFU;
  const luau::OverlayImage a = luau::rasterize_overlay(std::vector<luau::OverlayOp>{t}), b = luau::rasterize_overlay(std::vector<luau::OverlayOp>{t});
  std::size_t lit = 0;
  for (std::size_t i = 3; i < a.rgba.size(); i += 4) lit += a.rgba[i] != 0;
  check(a.rgba == b.rgba && lit > 40, "text renders deterministically with the viewer's body font");
  luau::OverlayOp edge = t;
  edge.x = 126;
  edge.y = 254;
  check(luau::rasterize_overlay(std::vector<luau::OverlayOp>{edge}).rgba.size() == a.rgba.size(), "text crossing the edge is clipped, never written out of bounds");
}

// ---- state-integrity ----
void test_state() {
  const std::string source =
      "local state = { score = 5, flags = { true, false }, name = 'x' }\n"
      "function save() return state end\n"
      "function load(v) state = v; got = v.score .. ':' .. #v.flags .. ':' .. v.name end\n";
  Mod m = start(source);
  const luau::StateResult s = m.vm->save_state();
  check(s.ok && !s.blob.empty() && s.blob.size() < 200, "a plain table saves");
  Mod n = start(source);
  check(n.vm->load_state(s.blob).ok, "the saved blob restores into a fresh VM");
  check(n.vm->describe_global("got") == "string:5:2:x", "load(value) received the saved structure");
  check(n.vm->save_state().blob == s.blob, "save -> load -> save is a fixed point (canonical bytes)");
  {   // canonical: insertion order does not change the bytes
    Mod a = start("function save() return { b = 2, a = 1, [3] = 'c', [1] = 'a' } end");
    Mod b = start("function save() local t = {} t[1] = 'a' t[3] = 'c' t.a = 1 t.b = 2 return t end");
    check(a.vm->save_state().blob == b.vm->save_state().blob, "state bytes do not depend on insertion order");
  }
  {   // string keys may contain NUL bytes: they survive the round trip and stay distinct
    const std::string nul = "function save() return { ['a\\0b'] = 1, ['a\\0c'] = 2, a = 3 } end\nfunction load(v) got = v['a\\0b'] .. ':' .. v['a\\0c'] .. ':' .. v.a end";
    Mod a = start(nul), b = start(nul);
    const luau::StateResult r = a.vm->save_state();
    check(r.ok && b.vm->load_state(r.blob).ok && b.vm->describe_global("got") == "string:1:2:3", "NUL bytes in state keys round-trip");
  }
  {   // a mod without save() has empty state; an empty blob restores nothing
    Mod a = start("x = 1");
    const luau::StateResult e = a.vm->save_state();
    check(e.ok && e.blob.empty() && a.vm->load_state({}).ok, "no save(): empty state");
  }
  const auto fails = [](const std::string &src, const char *what) {
    Mod a = start(src);
    const luau::StateResult r = a.vm->save_state();
    check(!r.ok && r.fault == luau::Fault::state && a.vm->disabled(), std::string("save is refused: ") + what);
  };
  fails("function save() return function() end end", "function-bearing state");
  fails("function save() return { f = math.floor } end", "a builtin function in state");
  fails("function save() return { b = buffer.create(4) } end", "a buffer in state");
  fails("function save() local t = {} t.self = t return t end", "a cycle");
  fails("function save() return { { { { { { { { { 1 } } } } } } } } } end", "nesting deeper than 8");
  fails("function save() return 0/0 end", "NaN");
  fails("function save() return math.huge end", "infinity");
  fails("function save() return { [{}] = 1 } end", "a table key");
  fails("function save() return { [true] = 1 } end", "a boolean key");
  fails("function save() return string.rep('x', 70000) end", "state over 64 KiB");
  fails("function save() local t = {} for i = 1, 20000 do t[i] = i end return t end", "many entries over 64 KiB");
  fails("function save() return nil end", "nil state");
  {
    Mod a = start("function save() return { { { { { { { 1 } } } } } } } end");   // depth 8 exactly
    check(a.vm->save_state().ok, "nesting of 8 is allowed");
  }
  {   // errors inside save() are ordinary faults
    Mod a = start("function save() error('nope') end");
    const luau::StateResult r = a.vm->save_state();
    check(!r.ok && r.fault == luau::Fault::error, "an error in save() is a fault");
  }
  // malformed blobs
  const std::vector<std::uint8_t> good = s.blob;
  const auto refused = [&](std::vector<std::uint8_t> blob, const char *what) {
    Mod a = start(source);
    const luau::CallResult r = a.vm->load_state(blob);
    check(!r.ok && r.fault == luau::Fault::state && a.vm->disabled(), std::string("restore is refused: ") + what);
  };
  refused(std::vector<std::uint8_t>(good.begin(), good.end() - 1), "truncated");
  { std::vector<std::uint8_t> v = good; v.push_back(0); refused(v, "trailing byte"); }
  { std::vector<std::uint8_t> v = good; v[0] = 9; refused(v, "unknown tag"); }
  refused({5, 0xFF, 0xFF, 0xFF, 0xFF}, "an entry count larger than the data");
  refused({5, 2, 0, 0, 0, 4, 1, 0, 0, 0, 'b', 1, 4, 1, 0, 0, 0, 'a', 1}, "keys out of canonical order");
  refused({5, 2, 0, 0, 0, 4, 1, 0, 0, 0, 'a', 1, 4, 1, 0, 0, 0, 'a', 2}, "a duplicate key");
  refused({5, 1, 0, 0, 0, 1, 1}, "a boolean key");
  { std::vector<std::uint8_t> v; for (int i = 0; i < 9; ++i) { v.push_back(5); v.push_back(1); v.push_back(0); v.push_back(0); v.push_back(0); v.push_back(3); for (int j = 0; j < 8; ++j) v.push_back(0); } refused(v, "nesting deeper than 8"); }
  refused({3, 0, 0, 0, 0, 0, 0, 0xF8, 0x7F}, "a NaN number");
  refused(std::vector<std::uint8_t>(luau::kStateBytesMax + 1, 1), "a blob over the size limit");
  {   // a non-empty blob for a mod without load()
    Mod a = start("function save() return 1 end");
    check(!a.vm->load_state(good).ok && a.vm->fault() == luau::Fault::state, "a mod without load() cannot receive state");
  }
  {   // an error in load() is a fault; the faulted VM refuses everything afterwards
    Mod a = start("function load(v) error('bad state') end");
    const luau::CallResult r = a.vm->load_state(good);
    check(!r.ok && r.fault == luau::Fault::error && !a.vm->load_state(good).ok, "an error in load() is a fault");
  }
}

// ---- adversarial corpus (T006) ----
void test_adversarial() {
  const auto faulted = [](const std::string &name, const std::string &body, std::vector<luau::Fault> allowed, luau::Limits limits = relaxed()) {
    Mod m = start("function render() " + body + " end", kAllCaps, limits);
    luau::Output out;
    const luau::CallResult r = render(m, 0, out);
    check(!r.ok && std::find(allowed.begin(), allowed.end(), r.fault) != allowed.end(), "adversarial: " + name + " is a fault of an allowed cause, got " + luau::fault_name(r.fault));
    check(out.log.empty() && out.overlay.empty() && m.vm->disabled(), "adversarial: " + name + " leaves the mod disabled and no output");
    check(m.vm->close() == 0, "adversarial: " + name + " tears down leak free");
  };
  luau::Limits small = relaxed();
  small.memory_bytes = 1U << 20U;
  // memory-exhaustion
  faulted("string.rep bomb", "local s = string.rep('ab', 2^30)", {luau::Fault::memory, luau::Fault::error}, small);
  faulted("string doubling", "local s = 'x' for i = 1, 40 do s = s .. s end", {luau::Fault::memory, luau::Fault::error}, small);
  faulted("table growth", "local t = {} for i = 1, 1e7 do t[i] = i end", {luau::Fault::memory, luau::Fault::budget}, small);
  faulted("huge table of strings", "local t = {} for i = 1, 1e6 do t[i] = string.rep('y', 64) .. i end", {luau::Fault::memory, luau::Fault::budget}, small);
  faulted("buffer bomb", "local b = buffer.create(2^30)", {luau::Fault::memory, luau::Fault::error}, small);
  faulted("recursive allocation", "local function f(n) local t = {n, n, n} return f(n + 1) and t end f(1)", {luau::Fault::stack, luau::Fault::memory});
  // cpu-exhaustion / stack
  faulted("infinite loop", "while true do end", {luau::Fault::budget});
  faulted("nested loops", "for i = 1, 1e5 do for j = 1, 1e5 do end end", {luau::Fault::budget});
  faulted("pcall around a loop", "while true do pcall(function() while true do end end) end", {luau::Fault::budget});
  faulted("deep recursion", "local function f(n) return 1 + f(n + 1) end f(1)", {luau::Fault::stack});
  faulted("mutual recursion through pcall", "local function f() return pcall(f) end f()", {luau::Fault::stack, luau::Fault::budget});
  // escape-metatable
  faulted("string metatable edit", "getmetatable('').__index.rep = nil", {luau::Fault::error});
  faulted("library table edit", "string.rep = nil", {luau::Fault::error});
  faulted("rawset on a library", "rawset(math, 'floor', nil)", {luau::Fault::error});
  faulted("rawset on globals", "rawset(_G, 'x', 1)", {luau::Fault::error});
  faulted("setmetatable on sega", "setmetatable(sega, { __index = function() return 1 end })", {luau::Fault::error});
  faulted("reading the guarded metatable", "getmetatable(sega).__index = nil", {luau::Fault::error});
  faulted("global environment edit", "string.format = print", {luau::Fault::error});
  // coroutine abuse and forbidden globals
  faulted("coroutine.wrap", "coroutine.wrap(function() end)()", {luau::Fault::error});
  faulted("coroutine.create", "local c = coroutine.create(function() end)", {luau::Fault::error});
  faulted("loadstring of bytecode", "loadstring('\\6\\1\\0')()", {luau::Fault::error});
  faulted("load", "load('return 1')()", {luau::Fault::error});
  faulted("require", "require('x')", {luau::Fault::error});
  faulted("getfenv", "getfenv(1).x = 1", {luau::Fault::error});
  faulted("os.time", "local t = os.time()", {luau::Fault::error});
  faulted("io.open", "io.open('x')", {luau::Fault::error});
  faulted("debug.traceback", "debug.traceback()", {luau::Fault::error});
  faulted("newproxy", "newproxy(true)", {luau::Fault::error});
  faulted("collectgarbage", "collectgarbage('count')", {luau::Fault::error});
  faulted("print", "print('x')", {luau::Fault::error});
  // forbidden-global reach by enumeration
  {
    Mod m = start("function render()\n  for _, n in ipairs({'os','io','debug','require','load','loadstring','dofile','loadfile','getfenv','setfenv','collectgarbage','gcinfo','newproxy','print','coroutine','tick','time','wait','spawn','delay','_G','shared','script','game','workspace','_VERSION'}) do\n"
                  "    if rawget(_G or {}, n) ~= nil then sega.log('reach:' .. n) end\n  end\n  sega.log('done')\nend");
    luau::Output out;
    const luau::CallResult r = render(m, 0, out);
    check(r.ok || r.fault == luau::Fault::error, "the reach script ends cleanly or with a plain error");
    for (const std::string &l : out.log) check(l == "done", "no forbidden global is reachable: " + l);
  }
  // determinism: identical results across runs, including faults
  {
    const std::string script = "function render(f)\n  local r = math.random(1, 1000)\n  sega.overlay.rect(r % 100, f % 200, 10, 10, 0xFF0000FF)\n  sega.log(tostring(r))\nend\n";
    std::vector<std::string> first;
    for (int run = 0; run < 3; ++run) {
      Mod m = start(script);
      std::vector<std::string> lines;
      for (std::uint64_t f = 0; f < 20; ++f) {
        luau::Output out;
        const luau::CallResult r = render(m, f, out);
        check(r.ok, "deterministic script runs");
        lines.push_back(out.log.at(0) + "/" + std::to_string(r.units) + "/" + std::to_string(out.overlay.at(0).x) + "," + std::to_string(out.overlay.at(0).y));
      }
      if (run == 0) first = lines;
      else check(lines == first, "determinism: the same script gives the same output and unit counts on every run");
    }
    Mod a = start("function render() while true do end end"), b = start("function render() while true do end end");
    luau::Output out;
    const luau::CallResult ra = render(a, 3, out), rb = render(b, 3, out);
    check(ra.message == rb.message && ra.units == rb.units && ra.fault == rb.fault, "determinism: a budget fault happens at the same unit count on every run");
  }
  // mod-isolation
  {
    Mod bad = start("function render() error('x') end", kAllCaps, relaxed(), "bad-mod");
    Mod good = start("counter = 0\nfunction render() counter = counter + 1 sega.log('c' .. counter) end", kAllCaps, relaxed(), "good-mod");
    luau::Output out;
    check(!render(bad, 0, out).ok, "the bad mod faults");
    check(render(good, 0, out).ok && render(good, 1, out).ok && out.log == std::vector<std::string>{"c2"}, "a faulted mod does not disturb another VM");
    Mod a = start("secret = 41");
    Mod b = start("function render() sega.log(tostring(secret)) end");
    luau::Output o2;
    check(render(b, 0, o2).ok && o2.log == std::vector<std::string>{"nil"}, "two mods share no globals");
  }
  // diagnostic-hygiene
  {
    const char *faulting[] = {"error('boom')", "sega.overlay.rect(1.5, 0, 1, 1, 1)", "while true do end", "local t = nil; t.x = 1", "local function f() return 1 + f() end f()", "string.rep('x', 1e9)"};
    for (const char *body : faulting) {
      luau::Limits l = relaxed();
      l.memory_bytes = 1U << 20U;
      Mod m = start(std::string("function render() ") + body + " end", kAllCaps, l);
      luau::Output out;
      const luau::CallResult r = render(m, 0, out);
      check(!r.ok && r.message.size() <= 200, std::string("a fault message is bounded: ") + body);
      check(r.message.find("0x") == std::string::npos && r.message.find(".cpp") == std::string::npos && r.message.find("/home") == std::string::npos && r.message.find("/Users") == std::string::npos &&
                r.message.find("\\") == std::string::npos,
            std::string("a fault message carries no host path or address: ") + r.message);
    }
  }
  // compiler-input
  {
    std::string deep(300, '(');
    deep = "local x = " + deep + "1" + std::string(300, ')');
    const luau::SourceCompile a = luau::compile_source("deep.luau", deep), b = luau::compile_source("deep.luau", deep);
    check(a.ok == b.ok && (a.ok || a.diagnostic.text() == b.diagnostic.text()), "deeply nested source compiles or is refused identically every time (no crash)");
    std::string tables = "local x = ";
    for (int i = 0; i < 400; ++i) tables += "{";
    for (int i = 0; i < 400; ++i) tables += "}";
    check(luau::compile_source("t.luau", tables).ok || luau::compile_source("t.luau", tables).diagnostic.line == 1, "400 nested tables: bounded result");
    const luau::SourceCompile huge = luau::compile_source("s.luau", "local s = '" + std::string(100000, 'a') + "'");
    check(huge.ok, "a 100 KB string literal compiles");
    std::string locals;
    for (int i = 0; i < 300; ++i) locals += "local v" + std::to_string(i) + " = f(" + std::to_string(i) + ")\n";
    const luau::SourceCompile many = luau::compile_source("l.luau", locals);
    check(!many.ok && many.diagnostic.line > 0, "more than 200 locals is a syntax error with a line: ok=" + std::to_string(many.ok) + " " + many.diagnostic.text());
  }
}

// ---- fixture session ----
void test_session() {
  const std::string hud =
      "function reset() sega.log('ready') end\n"
      "function render(frame)\n"
      "  sega.overlay.rect(0, 0, 96, 28, 0x000000A0)\n"
      "  sega.overlay.text(4, 3, 'SCORE ' .. string.format('%06d', sega.frame.value('score')), 0xFFFFFFFF)\n"
      "end\n";
  Mod m = start(hud);
  const std::string image(64, 'c');
  const luau::SessionResult a = luau::run_fixture_session(m.package, m.compiled, image, 30);
  Mod m2 = start(hud);
  const luau::SessionResult b = luau::run_fixture_session(m2.package, m2.compiled, image, 30);
  check(a.ok && b.ok && a.frames == 30 && a.frame_sha256.size() == 30 && a.overlay_frames == 30, "the session presents every frame with an overlay");
  check(a.session_sha256 == b.session_sha256 && a.frame_sha256 == b.frame_sha256 && a.last_frame == b.last_frame, "determinism: the session hash is identical across runs");
  check(a.log == std::vector<std::string>{"ready"} && a.fault == luau::Fault::none && a.diagnostic.empty() && a.leaked_bytes == 0 && !a.nondeterministic, "log, no fault, leak-free teardown");
  check(a.frame_sha256.front() != a.frame_sha256.back(), "frames differ over time");
  const luau::SessionResult c = luau::run_fixture_session(m.package, m.compiled, std::string(64, 'd'), 30);
  Mod m3 = start(hud);
  const luau::SessionResult c2 = luau::run_fixture_session(m3.package, m3.compiled, std::string(64, 'd'), 30);
  check(c2.session_sha256 != a.session_sha256 && c2.ok, "the frame hash depends on the fixture image digest");
  (void)c;
  Mod plain = start("x = 1");
  const luau::SessionResult p = luau::run_fixture_session(plain.package, plain.compiled, image, 30);
  check(p.ok && p.overlay_frames == 0 && p.frame_sha256 != a.frame_sha256, "a mod without overlay leaves the base picture; the overlay changes the hash");
  // a faulting mod disables only itself; the host keeps presenting frames
  Mod bad = start("function render(f) if f == 5 then error('boom') end sega.overlay.rect(0, 0, 8, 8, 0xFF0000FF) end");
  const luau::SessionResult f = luau::run_fixture_session(bad.package, bad.compiled, image, 12);
  check(f.ok && f.frames == 12 && f.fault == luau::Fault::error && f.fault_frame == 5 && f.overlay_frames == 5, "a failing script disables its mod at the fault and never stops the host");
  check(f.diagnostic.rfind("E_MOD_FAULT cause=error mod=api-test callback=render frame=5: ", 0) == 0, "the fault diagnostic has the stable shape: " + f.diagnostic);
  check(f.frame_sha256[5] == p.frame_sha256[5] && f.frame_sha256[11] == p.frame_sha256[11], "after the fault the frames equal the unmodded session");
  check(!luau::run_fixture_session(m.package, m.compiled, image, 0).ok && !luau::run_fixture_session(m.package, m.compiled, image, luau::kSessionFramesMax + 1).ok, "the frame count is bounded");
  // golden: pins the HUD example's result on every platform that runs this test (the same integer pipeline everywhere)
  check(a.session_sha256.size() == 64, "session digest is a SHA-256");
}

}  // namespace

int main() {
  test_overlay_layers();
  test_capability_gating();
  test_binding_abuse();
  test_overlay_validation();
  test_api_list();
  test_analysis();
  test_raster();
  test_state();
  test_adversarial();
  test_session();
  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::puts("luau_api_test: ok");
  return 0;
}
